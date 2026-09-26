#include <intravenous/runtime/graph_connections.h>

#include <intravenous/graph/builder/state.h>
#include <intravenous/graph/configured_graph.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace iv {
namespace {
struct PathCursor {
    enum class Kind { bundle, virtual_node } kind = Kind::bundle;
    NodeBundleHandle bundle = 0;
    VirtualNodeHandle virtual_node = 0;
    std::vector<VirtualNodeHandle> virtual_ancestry{};
};

bool subtree_contains(
    ConfiguredGraph const& graph,
    NodeBundleHandle root,
    NodeBundleHandle candidate)
{
    if (root == candidate) return true;
    auto const& bundle = graph.node_bundles.bundle(root);
    if (bundle.is_subgraph()) {
        auto const begin = bundle.subgraph_child_begin();
        auto const end = begin + bundle.subgraph_child_count();
        return candidate >= begin && candidate < end;
    }
    if (bundle.is_tiled()) {
        return std::ranges::any_of(bundle.tiled_members(), [&](auto member) {
            return subtree_contains(graph, member, candidate);
        });
    }
    return false;
}

std::vector<NodeBundleHandle> direct_subgraphs(
    ConfiguredGraph const& graph,
    NodeBundleHandle root)
{
    std::vector<NodeBundleHandle> candidates;
    for (NodeBundleHandle handle = 0; handle < graph.node_bundles.size(); ++handle) {
        if (handle == root || !graph.node_bundles.bundle(handle).is_subgraph()) continue;
        if (subtree_contains(graph, root, handle)) candidates.push_back(handle);
    }
    std::erase_if(candidates, [&](NodeBundleHandle candidate) {
        return std::ranges::any_of(candidates, [&](NodeBundleHandle possible_parent) {
            return possible_parent != candidate
                && subtree_contains(graph, possible_parent, candidate);
        });
    });
    std::ranges::sort(candidates);
    return candidates;
}

std::vector<NodeBundleHandle> cursor_roots(
    ConfiguredGraph const& graph,
    PathCursor const& cursor)
{
    if (cursor.kind == PathCursor::Kind::bundle) return {cursor.bundle};
    auto const& members = graph.virtual_nodes.record(cursor.virtual_node).node_bundle_handles;
    return {members.begin(), members.end()};
}

bool virtual_selector_matches(
    VirtualNodeRecord const& record,
    ProjectVirtualNodeSelector const& selector)
{
    if (record.source_identity != selector.source_identity) return false;
    return !selector.type_identity.has_value()
        || record.type_identity == *selector.type_identity;
}

void deduplicate_cursors(std::vector<PathCursor>& cursors)
{
    std::vector<PathCursor> unique;
    unique.reserve(cursors.size());
    for (auto& cursor : cursors) {
        auto const duplicate = std::ranges::any_of(unique, [&](PathCursor const& existing) {
            return existing.kind == cursor.kind
                && (cursor.kind == PathCursor::Kind::bundle
                    ? existing.bundle == cursor.bundle
                    : existing.virtual_node == cursor.virtual_node);
        });
        if (!duplicate) unique.push_back(std::move(cursor));
    }
    cursors = std::move(unique);
}

std::vector<PathCursor> apply_path_selector(
    ConfiguredGraph const& graph,
    std::vector<PathCursor> const& current,
    ProjectNodePathSelector const& selector)
{
    std::vector<PathCursor> next;
    std::visit([&](auto const& step) {
        using Step = std::remove_cvref_t<decltype(step)>;
        if constexpr (std::same_as<Step, ProjectVirtualNodeSelector>) {
            for (auto const& cursor : current) {
                auto const roots = cursor_roots(graph, cursor);
                for (VirtualNodeHandle handle = 0;
                     handle < graph.virtual_nodes.records().size(); ++handle) {
                    if (cursor.kind == PathCursor::Kind::virtual_node
                        && handle == cursor.virtual_node) {
                        continue;
                    }
                    if (std::ranges::contains(cursor.virtual_ancestry, handle)) continue;
                    auto const& record = graph.virtual_nodes.record(handle);
                    if (!virtual_selector_matches(record, step)) continue;
                    auto const under_cursor = std::ranges::any_of(
                        record.node_bundle_handles, [&](NodeBundleHandle member) {
                            return std::ranges::any_of(roots, [&](NodeBundleHandle root) {
                                return subtree_contains(graph, root, member);
                            });
                        });
                    if (!under_cursor) continue;
                    auto ancestry = cursor.virtual_ancestry;
                    if (cursor.kind == PathCursor::Kind::virtual_node)
                        ancestry.push_back(cursor.virtual_node);
                    next.push_back(PathCursor{
                        .kind = PathCursor::Kind::virtual_node,
                        .virtual_node = handle,
                        .virtual_ancestry = std::move(ancestry),
                    });
                }
            }
        } else if constexpr (std::same_as<Step, ProjectVirtualMemberSelector>) {
            for (auto const& cursor : current) {
                if (cursor.kind != PathCursor::Kind::virtual_node) continue;
                auto const& record = graph.virtual_nodes.record(cursor.virtual_node);
                if (step.index >= record.node_bundle_handles.size()) continue;
                auto ancestry = cursor.virtual_ancestry;
                ancestry.push_back(cursor.virtual_node);
                next.push_back(PathCursor{
                    .kind = PathCursor::Kind::bundle,
                    .bundle = record.node_bundle_handles[step.index],
                    .virtual_ancestry = std::move(ancestry),
                });
            }
        } else if constexpr (std::same_as<Step, ProjectTiledChildSelector>) {
            for (auto const& cursor : current) {
                if (cursor.kind != PathCursor::Kind::bundle) continue;
                auto const& bundle = graph.node_bundles.bundle(cursor.bundle);
                if (!bundle.is_tiled()) continue;
                auto const members = bundle.tiled_members();
                if (step.index >= members.size()) continue;
                next.push_back(PathCursor{
                    .kind = PathCursor::Kind::bundle,
                    .bundle = members[step.index],
                    .virtual_ancestry = cursor.virtual_ancestry,
                });
            }
        } else if constexpr (std::same_as<Step, ProjectSubgraphSelector>) {
            for (auto const& cursor : current) {
                auto const roots = cursor_roots(graph, cursor);
                for (auto const root : roots) {
                    auto subgraphs = direct_subgraphs(graph, root);
                    if (step.kind.has_value()) {
                        std::erase_if(subgraphs, [&](NodeBundleHandle handle) {
                            return graph.node_bundles.bundle(handle).subgraph_kind() != *step.kind;
                        });
                    }
                    if (step.index >= subgraphs.size()) continue;
                    next.push_back(PathCursor{
                        .kind = PathCursor::Kind::bundle,
                        .bundle = subgraphs[step.index],
                        .virtual_ancestry = cursor.virtual_ancestry,
                    });
                }
            }
        }
    }, selector);
    deduplicate_cursors(next);
    return next;
}

std::vector<PathCursor> resolve_path(
    ConfiguredGraph const& graph,
    NodeInstancePlacement const& placement,
    ProjectNodePortMatcher const& matcher)
{
    std::vector<PathCursor> current{{
        .kind = PathCursor::Kind::bundle,
        .bundle = placement.root,
    }};
    for (auto const& selector : matcher.path) {
        current = apply_path_selector(graph, current, selector);
        if (current.empty()) break;
    }
    return current;
}

bool port_matches(
    ProjectPortMatcher const& matcher,
    std::string_view name,
    std::size_t index)
{
    if (matcher.name.has_value() && name != *matcher.name) return false;
    if (matcher.index.has_value() && index != *matcher.index) return false;
    return true;
}

struct ResolvedSampleOutput {
    ChannelTypeId type = ChannelTypeId::mono;
    std::vector<SampleOutputChannelId> channels{};
    bool operator==(ResolvedSampleOutput const&) const = default;
};

struct ResolvedSampleInput {
    ChannelTypeId type = ChannelTypeId::mono;
    std::variant<NodeBundlePortId, SampleInputChannelId> target{};
    bool operator==(ResolvedSampleInput const&) const = default;
};

struct ResolvedEventOutput {
    EventTypeId type = EventTypeId::empty;
    std::vector<EventOutputPortId> sources{};
    bool operator==(ResolvedEventOutput const&) const = default;
};

struct ResolvedEventInput {
    EventTypeId type = EventTypeId::empty;
    NodeBundlePortId target{};
    bool operator==(ResolvedEventInput const&) const = default;
};

template<class T>
void append_unique(std::vector<T>& output, T value)
{
    if (!std::ranges::contains(output, value)) output.push_back(std::move(value));
}

std::vector<ResolvedSampleOutput> sample_outputs_for_cursor(
    ConfiguredGraph const& graph,
    PathCursor const& cursor,
    ProjectPortMatcher const& matcher)
{
    std::vector<ResolvedSampleOutput> result;
    if (cursor.kind == PathCursor::Kind::virtual_node) {
        auto const& record = graph.virtual_nodes.record(cursor.virtual_node);
        for (auto const& mapping : record.sample_outputs) {
            if (!port_matches(matcher, mapping.name, mapping.index)) continue;
            for (auto const& member_channels : mapping.member_channels) {
                if (matcher.channel.has_value()) {
                    if (*matcher.channel >= member_channels.size()) continue;
                    append_unique(result, ResolvedSampleOutput{
                        .type = ChannelTypeId::mono,
                        .channels = {member_channels[*matcher.channel]},
                    });
                } else {
                    append_unique(result, ResolvedSampleOutput{
                        .type = mapping.channel_layout.channel_type,
                        .channels = member_channels,
                    });
                }
            }
        }
        return result;
    }

    auto const& bundle = graph.node_bundles.bundle(cursor.bundle);
    for (std::size_t index = 0; index < bundle.sample_output_count(); ++index) {
        auto const config = graph.node_bundles.resolve_sample_output(
            {cursor.bundle, PortKind::sample, index}).config;
        if (!port_matches(matcher, config.name, index)) continue;
        auto channels = graph.node_bundles.sample_output_channels(
            {cursor.bundle, PortKind::sample, index});
        if (matcher.channel.has_value()) {
            if (*matcher.channel >= channels.size()) continue;
            append_unique(result, ResolvedSampleOutput{
                .type = ChannelTypeId::mono,
                .channels = {channels[*matcher.channel]},
            });
        } else {
            append_unique(result, ResolvedSampleOutput{
                .type = config.channel_layout.channel_type,
                .channels = std::move(channels),
            });
        }
    }
    return result;
}

std::vector<ResolvedSampleInput> sample_inputs_for_cursor(
    ConfiguredGraph const& graph,
    PathCursor const& cursor,
    ProjectPortMatcher const& matcher)
{
    std::vector<ResolvedSampleInput> result;
    if (cursor.kind == PathCursor::Kind::virtual_node) {
        auto const& record = graph.virtual_nodes.record(cursor.virtual_node);
        for (auto const& mapping : record.sample_inputs) {
            if (!port_matches(matcher, mapping.name, mapping.index)) continue;
            for (auto const& member_channels : mapping.member_channels) {
                if (member_channels.empty()) continue;
                if (matcher.channel.has_value()) {
                    if (*matcher.channel >= member_channels.size()) continue;
                    append_unique(result, ResolvedSampleInput{
                        .type = ChannelTypeId::mono,
                        .target = member_channels[*matcher.channel],
                    });
                } else {
                    auto const first = member_channels.front();
                    auto const one_port = std::ranges::all_of(
                        member_channels, [&](SampleInputChannelId channel) {
                            return channel.bundle == first.bundle && channel.port == first.port;
                        });
                    if (!one_port) {
                        throw std::logic_error(
                            "virtual sample input member spans multiple bundle ports");
                    }
                    append_unique(result, ResolvedSampleInput{
                        .type = mapping.channel_layout.channel_type,
                        .target = NodeBundlePortId{
                            first.bundle, PortKind::sample, first.port},
                    });
                }
            }
        }
        return result;
    }

    auto const& bundle = graph.node_bundles.bundle(cursor.bundle);
    for (std::size_t index = 0; index < bundle.sample_input_count(); ++index) {
        auto const config = graph.node_bundles.resolve_sample_input(
            {cursor.bundle, PortKind::sample, index}).config;
        if (!port_matches(matcher, config.name, index)) continue;
        auto channels = graph.node_bundles.sample_input_channels(
            {cursor.bundle, PortKind::sample, index});
        if (matcher.channel.has_value()) {
            if (*matcher.channel >= channels.size()) continue;
            append_unique(result, ResolvedSampleInput{
                .type = ChannelTypeId::mono,
                .target = channels[*matcher.channel],
            });
        } else {
            append_unique(result, ResolvedSampleInput{
                .type = config.channel_layout.channel_type,
                .target = NodeBundlePortId{cursor.bundle, PortKind::sample, index},
            });
        }
    }
    return result;
}

std::vector<ResolvedEventOutput> event_outputs_for_cursor(
    ConfiguredGraph const& graph,
    PathCursor const& cursor,
    ProjectPortMatcher const& matcher)
{
    if (matcher.channel.has_value()) return {};
    std::vector<ResolvedEventOutput> result;
    if (cursor.kind == PathCursor::Kind::virtual_node) {
        auto const& record = graph.virtual_nodes.record(cursor.virtual_node);
        for (auto const& mapping : record.event_outputs) {
            if (!port_matches(matcher, mapping.name, mapping.index)) continue;
            for (auto const port : mapping.node_bundle_ports) {
                append_unique(result, ResolvedEventOutput{
                    .type = mapping.type,
                    .sources = graph.node_bundles.event_output_ports(port),
                });
            }
        }
        return result;
    }

    auto const& bundle = graph.node_bundles.bundle(cursor.bundle);
    for (std::size_t index = 0; index < bundle.event_output_count(); ++index) {
        auto const config = graph.node_bundles.resolve_event_output(
            {cursor.bundle, PortKind::event, index}).config;
        if (!port_matches(matcher, config.name, index)) continue;
        append_unique(result, ResolvedEventOutput{
            .type = config.type,
            .sources = graph.node_bundles.event_output_ports(
                {cursor.bundle, PortKind::event, index}),
        });
    }
    return result;
}

std::vector<ResolvedEventInput> event_inputs_for_cursor(
    ConfiguredGraph const& graph,
    PathCursor const& cursor,
    ProjectPortMatcher const& matcher)
{
    if (matcher.channel.has_value()) return {};
    std::vector<ResolvedEventInput> result;
    if (cursor.kind == PathCursor::Kind::virtual_node) {
        auto const& record = graph.virtual_nodes.record(cursor.virtual_node);
        for (auto const& mapping : record.event_inputs) {
            if (!port_matches(matcher, mapping.name, mapping.index)) continue;
            for (auto const port : mapping.node_bundle_ports) {
                append_unique(result, ResolvedEventInput{
                    .type = mapping.type,
                    .target = port,
                });
            }
        }
        return result;
    }

    auto const& bundle = graph.node_bundles.bundle(cursor.bundle);
    for (std::size_t index = 0; index < bundle.event_input_count(); ++index) {
        auto const config = graph.node_bundles.resolve_event_input(
            {cursor.bundle, PortKind::event, index}).config;
        if (!port_matches(matcher, config.name, index)) continue;
        append_unique(result, ResolvedEventInput{
            .type = config.type,
            .target = {cursor.bundle, PortKind::event, index},
        });
    }
    return result;
}

template<class Result, class Resolver>
std::vector<Result> resolve_matcher(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    ProjectNodePortMatcher const& matcher,
    Resolver&& resolver)
{
    auto const placement = placements.find(matcher.instance_id);
    if (placement == placements.end()) return {};
    auto const cursors = resolve_path(graph, placement->second, matcher);
    std::vector<Result> result;
    for (auto const& cursor : cursors) {
        auto values = std::forward<Resolver>(resolver)(graph, cursor, matcher.port);
        for (auto& value : values) append_unique(result, std::move(value));
    }
    return result;
}

void validate_matcher(ProjectNodePortMatcher const& matcher)
{
    if (matcher.instance_id.empty())
        throw std::invalid_argument("project connection matcher has an empty instance id");
    if (matcher.port.name.has_value() && matcher.port.name->empty())
        throw std::invalid_argument("project connection matcher has an empty port name");
    for (auto const& selector : matcher.path) {
        if (auto const* virtual_node = std::get_if<ProjectVirtualNodeSelector>(&selector);
            virtual_node && virtual_node->source_identity.empty()) {
            throw std::invalid_argument(
                "project virtual-node selector has an empty source identity");
        }
        if (auto const* subgraph = std::get_if<ProjectSubgraphSelector>(&selector);
            subgraph && subgraph->kind.has_value() && subgraph->kind->empty()) {
            throw std::invalid_argument("project subgraph selector has an empty kind");
        }
    }
}

void validate_connection(ProjectConnection const& connection)
{
    std::visit([](auto const& value) {
        if (value.connection_id.empty())
            throw std::invalid_argument("project connection id must not be empty");
        if (value.outputs.empty())
            throw std::invalid_argument("project connection requires at least one output matcher");
        if (value.inputs.empty())
            throw std::invalid_argument("project connection requires at least one input matcher");
        for (auto const& matcher : value.outputs) validate_matcher(matcher);
        for (auto const& matcher : value.inputs) validate_matcher(matcher);
    }, connection);
}

std::unordered_map<std::string, ProjectConnection> apply_mutation(
    std::unordered_map<std::string, ProjectConnection> desired,
    GraphConnectionsMutation const& mutation)
{
    std::visit([&](auto const& value) {
        using Mutation = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<Mutation, std::monostate>) {
            return;
        } else if constexpr (std::same_as<Mutation, GraphConnectionUpsertMutation>) {
            validate_connection(value.connection);
            desired[project_connection_id(value.connection)] = value.connection;
        } else if constexpr (std::same_as<Mutation, GraphConnectionDeleteMutation>) {
            if (value.connection_id.empty())
                throw std::invalid_argument("project connection id must not be empty");
            if (desired.erase(value.connection_id) == 0)
                throw std::runtime_error("unknown project connection: " + value.connection_id);
        } else if constexpr (std::same_as<Mutation, GraphConnectionReplaceAllMutation>) {
            std::unordered_map<std::string, ProjectConnection> replacement;
            replacement.reserve(value.connections.size());
            for (auto const& connection : value.connections) {
                validate_connection(connection);
                auto const& id = project_connection_id(connection);
                if (!replacement.emplace(id, connection).second)
                    throw std::invalid_argument(
                        "duplicate project connection id in replacement batch: " + id);
            }
            desired = std::move(replacement);
        }
    }, mutation);
    return desired;
}

void add_unresolved_diagnostic(
    GraphConnectionsBatchResult& result,
    std::string const& connection_id,
    std::string_view side,
    std::size_t matcher_index)
{
    result.diagnostics.push_back(GraphConnectionDiagnostic{
        .connection_id = connection_id,
        .message = std::string(side) + " matcher " + std::to_string(matcher_index)
            + " resolved to no ports",
    });
}

void apply_sample_connection(
    ProjectSampleConnection const& connection,
    ConfiguredGraph const& snapshot,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    GraphBuilder& root_builder,
    GraphConnectionsBatchResult& result)
{
    std::vector<ResolvedSampleOutput> outputs;
    std::vector<ResolvedSampleInput> inputs;
    for (std::size_t i = 0; i < connection.outputs.size(); ++i) {
        auto resolved = resolve_matcher<ResolvedSampleOutput>(
            snapshot, placements, connection.outputs[i], sample_outputs_for_cursor);
        if (resolved.empty()) add_unresolved_diagnostic(result, connection.connection_id, "output", i);
        for (auto& value : resolved) append_unique(outputs, std::move(value));
    }
    for (std::size_t i = 0; i < connection.inputs.size(); ++i) {
        auto resolved = resolve_matcher<ResolvedSampleInput>(
            snapshot, placements, connection.inputs[i], sample_inputs_for_cursor);
        if (resolved.empty()) add_unresolved_diagnostic(result, connection.connection_id, "input", i);
        for (auto& value : resolved) append_unique(inputs, std::move(value));
    }
    if (outputs.empty() || inputs.empty()) return;

    auto const source_mismatch = std::ranges::find_if(outputs, [&](auto const& output) {
        return output.type != connection.source_type;
    });
    if (source_mismatch != outputs.end()) {
        result.diagnostics.push_back({
            .connection_id = connection.connection_id,
            .message = "resolved sample output channel type does not match connection source type",
        });
        return;
    }
    auto const target_mismatch = std::ranges::find_if(inputs, [&](auto const& input) {
        return input.type != connection.target_type;
    });
    if (target_mismatch != inputs.end()) {
        result.diagnostics.push_back({
            .connection_id = connection.connection_id,
            .message = "resolved sample input channel type does not match connection target type",
        });
        return;
    }

    auto& state = details::builder_graph_state(root_builder);
    for (auto const& input : inputs) {
        for (auto const& output : outputs) {
            SamplePortRef source(root_builder, output.type, output.channels);
            std::visit([&](auto const& target) {
                state.connect_sample_input(target, source);
            }, input.target);
        }
    }
    result.applied_connection_ids.push_back(connection.connection_id);
}

void apply_event_connection(
    ProjectEventConnection const& connection,
    ConfiguredGraph const& snapshot,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    GraphBuilder& root_builder,
    GraphConnectionsBatchResult& result)
{
    std::vector<ResolvedEventOutput> outputs;
    std::vector<ResolvedEventInput> inputs;
    for (std::size_t i = 0; i < connection.outputs.size(); ++i) {
        auto resolved = resolve_matcher<ResolvedEventOutput>(
            snapshot, placements, connection.outputs[i], event_outputs_for_cursor);
        if (resolved.empty()) add_unresolved_diagnostic(result, connection.connection_id, "output", i);
        for (auto& value : resolved) append_unique(outputs, std::move(value));
    }
    for (std::size_t i = 0; i < connection.inputs.size(); ++i) {
        auto resolved = resolve_matcher<ResolvedEventInput>(
            snapshot, placements, connection.inputs[i], event_inputs_for_cursor);
        if (resolved.empty()) add_unresolved_diagnostic(result, connection.connection_id, "input", i);
        for (auto& value : resolved) append_unique(inputs, std::move(value));
    }
    if (outputs.empty() || inputs.empty()) return;

    if (std::ranges::any_of(outputs, [&](auto const& output) {
            return output.type != connection.source_type;
        })) {
        result.diagnostics.push_back({
            .connection_id = connection.connection_id,
            .message = "resolved event output type does not match connection source type",
        });
        return;
    }
    if (std::ranges::any_of(inputs, [&](auto const& input) {
            return input.type != connection.target_type;
        })) {
        result.diagnostics.push_back({
            .connection_id = connection.connection_id,
            .message = "resolved event input type does not match connection target type",
        });
        return;
    }

    auto& state = details::builder_graph_state(root_builder);
    for (auto const& input : inputs) {
        for (auto const& output : outputs) {
            EventPortRef source(root_builder, output.type, output.sources);
            state.connect_event_input(input.target, source);
        }
    }
    result.applied_connection_ids.push_back(connection.connection_id);
}
} // namespace

std::vector<ProjectConnection> GraphConnections::desired_connections() const
{
    std::scoped_lock lock(mutex_);
    std::vector<ProjectConnection> result;
    result.reserve(desired_connections_.size());
    for (auto const& [_, connection] : desired_connections_) result.push_back(connection);
    std::ranges::sort(result, {}, [](ProjectConnection const& connection) {
        return project_connection_id(connection);
    });
    return result;
}

void GraphConnections::handle_project_graph_transaction(
    GraphConnectionsProjectGraphRequest& request)
{
    if (request.handled)
        throw std::logic_error("GraphConnections ProjectGraph request was handled more than once");
    request.handled = true;
    if (!request.root_builder)
        throw std::invalid_argument("GraphConnections transaction requires a root GraphBuilder");
    if (!request.placements)
        throw std::invalid_argument("GraphConnections transaction requires instance placements");

    std::scoped_lock lock(mutex_);
    auto staged = apply_mutation(desired_connections_, request.mutation);

    // Resolve against an immutable view of exactly the complete root produced
    // by NodeInstances before any project connection is added. Handles in this
    // snapshot are identical to the live builder handles used below.
    auto const snapshot = request.root_builder->finish();

    std::vector<std::string> ordered_ids;
    ordered_ids.reserve(staged.size());
    for (auto const& [id, _] : staged) ordered_ids.push_back(id);
    std::ranges::sort(ordered_ids);

    GraphConnectionsBatchResult result;
    for (auto const& id : ordered_ids) {
        auto const& connection = staged.at(id);
        try {
            std::visit([&](auto const& value) {
                using Connection = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Connection, ProjectSampleConnection>) {
                    apply_sample_connection(
                        value, snapshot, *request.placements, *request.root_builder, result);
                } else {
                    apply_event_connection(
                        value, snapshot, *request.placements, *request.root_builder, result);
                }
            }, connection);
        } catch (std::exception const& error) {
            result.diagnostics.push_back({
                .connection_id = id,
                .message = error.what(),
            });
        } catch (...) {
            result.diagnostics.push_back({
                .connection_id = id,
                .message = "unknown project connection resolution failure",
            });
        }
    }

    desired_connections_ = std::move(staged);
    request.result = std::move(result);
}
} // namespace iv
