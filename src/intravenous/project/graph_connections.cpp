#include <intravenous/project/graph_connections.h>

#include <intravenous/graph/builder/state.h>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/project/project_node_path_resolver.h>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace iv {
namespace {
using PathCursor = ProjectNodePathMatch;

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
    std::vector<SampleOutputRoute> routes{};
    bool ambiguous_occurrence = false;
    bool operator==(ResolvedSampleOutput const&) const = default;
};

struct ResolvedSampleInput {
    ChannelTypeId type = ChannelTypeId::mono;
    std::variant<NodeBundlePortId, SampleInputChannelId> target{};
    std::optional<SymbolicNodeHandle> symbolic_node{};
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
    auto append = [&](ChannelTypeId type, std::vector<SampleOutputRoute> routes,
                      bool ambiguous_occurrence = false) {
        if (matcher.channel) {
            if (*matcher.channel >= routes.size()) return;
            routes = {routes[*matcher.channel]};
            type = ChannelTypeId::mono;
        }
        if (routes.size() != channel_count(type)) return;
        auto const existing = std::ranges::find_if(result, [&](auto const& output) {
            return output.type == type && std::ranges::equal(output.routes, routes, {},
                &SampleOutputRoute::channel, &SampleOutputRoute::channel);
        });
        if (existing != result.end()) {
            existing->ambiguous_occurrence |= ambiguous_occurrence || existing->routes != routes;
        } else {
            result.push_back({type, std::move(routes), ambiguous_occurrence});
        }
    };
    if (cursor.kind == PathCursor::Kind::symbolic_node) {
        auto const& record = graph.symbolic_nodes.record(cursor.symbolic_node);
        if (record.type_identity == "sample-port") {
            if (record.sample_outputs.empty()) return result;
            auto const& mapping = record.sample_outputs.front();
            if (!port_matches(matcher, mapping.name, mapping.index)) return result;
            for (auto const& expression : record.sample_output_routes) {
                std::vector<SampleOutputRoute> selected;
                for (auto const& route : expression) {
                    auto const member = std::ranges::find(record.node_bundle_handles, route.channel.bundle);
                    if (member != record.node_bundle_handles.end()
                        && std::ranges::contains(cursor.symbolic_member_indices,
                            static_cast<std::size_t>(member - record.node_bundle_handles.begin()))) {
                        selected.push_back(route);
                    }
                }
                append(mapping.channel_layout.channel_type, std::move(selected));
            }
            return result;
        }
        for (auto const& mapping : record.sample_outputs) {
            if (!port_matches(matcher, mapping.name, mapping.index)) continue;
            for (auto const member : cursor.symbolic_member_indices) {
                if (member >= mapping.member_channels.size()) continue;
                auto const& channels = mapping.member_channels[member];
                if (channels.empty()) continue;
                auto const occurrences = graph.node_bundles.bundle(channels.front().bundle).node_ref_occurrences();
                std::optional<std::size_t> selected;
                bool ambiguous = false;
                for (std::size_t index = 0; index < occurrences.size(); ++index) {
                    if (occurrences[index].symbolic_node_handle != cursor.symbolic_node) continue;
                    if (selected) {
                        ambiguous = true;
                        break;
                    }
                    selected = index;
                }
                std::vector<SampleOutputRoute> routes;
                routes.reserve(channels.size());
                for (auto channel : channels) routes.push_back({channel, selected});
                append(mapping.channel_layout.channel_type, std::move(routes), ambiguous);
            }
        }
        return result;
    }

    auto const& bundle = graph.node_bundles.bundle(cursor.bundle);
    for (std::size_t index = 0; index < bundle.sample_output_count(); ++index) {
        auto const port = NodeBundlePortId{cursor.bundle, PortKind::sample, index};
        auto const config = graph.node_bundles.resolve_sample_output(port).config;
        if (!port_matches(matcher, config.name, index)) continue;
        std::vector<SampleOutputRoute> routes;
        for (auto channel : graph.node_bundles.sample_output_channels(port)) routes.push_back({channel});
        append(config.channel_layout.channel_type, std::move(routes));
    }
    return result;
}

std::vector<ResolvedSampleInput> sample_inputs_for_cursor(
    ConfiguredGraph const& graph,
    PathCursor const& cursor,
    ProjectPortMatcher const& matcher)
{
    std::vector<ResolvedSampleInput> result;
    if (cursor.kind == PathCursor::Kind::symbolic_node) {
        auto const& record = graph.symbolic_nodes.record(cursor.symbolic_node);
        for (auto const& mapping : record.sample_inputs) {
            if (!port_matches(matcher, mapping.name, mapping.index)) continue;
            for (auto member : cursor.symbolic_member_indices) {
                if (member >= mapping.member_channels.size()) continue;
                auto const& member_channels = mapping.member_channels[member];
                if (member_channels.empty()) continue;
                if (matcher.channel.has_value()) {
                    if (*matcher.channel >= member_channels.size()) continue;
                    append_unique(result, ResolvedSampleInput{
                        .type = ChannelTypeId::mono,
                        .target = member_channels[*matcher.channel],
                        .symbolic_node = cursor.symbolic_node,
                    });
                } else {
                    if (member_channels.size() != channel_type_info(
                            mapping.channel_layout.channel_type).channel_count) {
                        continue;
                    }
                    auto const first = member_channels.front();
                    auto const one_port = std::ranges::all_of(
                        member_channels, [&](SampleInputChannelId channel) {
                            return channel.bundle == first.bundle && channel.port == first.port;
                        });
                    if (!one_port) {
                        throw std::logic_error(
                            "symbolic sample input member spans multiple bundle ports");
                    }
                    append_unique(result, ResolvedSampleInput{
                        .type = mapping.channel_layout.channel_type,
                        .target = NodeBundlePortId{
                            first.bundle, PortKind::sample, first.port},
                        .symbolic_node = cursor.symbolic_node,
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
    if (cursor.kind == PathCursor::Kind::symbolic_node) {
        auto const& record = graph.symbolic_nodes.record(cursor.symbolic_node);
        for (auto const& mapping : record.event_outputs) {
            if (!port_matches(matcher, mapping.name, mapping.index)) continue;
            for (auto const port : mapping.node_bundle_ports) {
                auto const selected = std::ranges::any_of(
                    cursor.symbolic_member_indices,
                    [&](std::size_t member_index) {
                        return member_index < record.node_bundle_handles.size()
                            && record.node_bundle_handles[member_index]
                                == port.node_bundle_handle;
                    });
                if (!selected) continue;
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
    if (cursor.kind == PathCursor::Kind::symbolic_node) {
        auto const& record = graph.symbolic_nodes.record(cursor.symbolic_node);
        for (auto const& mapping : record.event_inputs) {
            if (!port_matches(matcher, mapping.name, mapping.index)) continue;
            for (auto const port : mapping.node_bundle_ports) {
                auto const selected = std::ranges::any_of(
                    cursor.symbolic_member_indices,
                    [&](std::size_t member_index) {
                        return member_index < record.node_bundle_handles.size()
                            && record.node_bundle_handles[member_index]
                                == port.node_bundle_handle;
                    });
                if (!selected) continue;
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
    auto const cursors = resolve_project_node_path(
        graph, placements, matcher.instance_id, matcher.path);
    std::vector<Result> result;
    for (auto cursor : cursors) {
        if (matcher.module_boundary) {
            if (cursor.kind != PathCursor::Kind::bundle) continue;
            auto const boundary = graph.node_bundles.bundle(cursor.bundle).subgraph_boundary_handle();
            if (!boundary) continue;
            cursor.bundle = *boundary;
        }
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
        if (auto const* symbolic_node = std::get_if<ProjectSymbolicNodeSelector>(&selector);
            symbolic_node && symbolic_node->source_identity.empty()) {
            throw std::invalid_argument(
                "project symbolic-node selector has an empty source identity");
        }
        if (auto const* tiled = std::get_if<ProjectTiledChildSelector>(&selector);
            tiled && tiled->name.empty()) {
            throw std::invalid_argument(
                "project tiled-child selector has an empty channel name");
        }
    }
}

void validate_connection_impl(ProjectConnection const& connection)
{
    std::visit([](auto const& value) {
        using Connection = std::remove_cvref_t<decltype(value)>;
        if (value.connection_id.empty())
            throw std::invalid_argument("project connection id must not be empty");
        if (value.outputs.empty())
            throw std::invalid_argument("project connection requires at least one output matcher");
        if (value.inputs.empty())
            throw std::invalid_argument("project connection requires at least one input matcher");
        if constexpr (std::same_as<Connection, ProjectSampleConnection>) {
            if (!is_registered_channel_type(value.source_type)
                || !is_registered_channel_type(value.target_type)) {
                throw std::invalid_argument(
                    "project sample connection uses an invalid channel type");
            }
        } else {
            if (value.source_type >= EventTypeId::count
                || value.target_type >= EventTypeId::count) {
                throw std::invalid_argument(
                    "project event connection uses an invalid event type");
            }
            auto const has_channel_selector = [](auto const& matcher) {
                return matcher.port.channel.has_value();
            };
            if (std::ranges::any_of(value.outputs, has_channel_selector)
                || std::ranges::any_of(value.inputs, has_channel_selector)) {
                throw std::invalid_argument(
                    "project event connection cannot use a port-channel selector");
            }
        }
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
            validate_project_connection(value.connection);
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
                validate_project_connection(connection);
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
        if (std::ranges::any_of(resolved, &ResolvedSampleOutput::ambiguous_occurrence)) {
            result.diagnostics.push_back({.connection_id = connection.connection_id,
                .message = "sample output matches multiple forwarding occurrences"});
            return;
        }
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

    std::vector<ConfiguredSampleConnectionSource> sources;
    sources.reserve(outputs.size() * channel_count(connection.source_type));
    for (auto const& output : outputs) {
        for (auto const& route : output.routes)
            sources.push_back({.channel = route.channel, .node_ref_occurrence = route.node_ref_occurrence});
    }
    auto& state = details::builder_graph_state(root_builder);
    auto const begin = state.mark().sample_connections;
    for (auto const& input : inputs) {
        state.connect_sample_sources(input.target, connection.source_type, sources, input.symbolic_node);
    }
    result.applied_connections.push_back({connection.connection_id, PortKind::sample, begin, state.mark().sample_connections});
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
    auto const begin = state.mark().event_connections;
    for (auto const& input : inputs) {
        for (auto const& output : outputs) {
            EventPortRef source(root_builder, output.type, output.sources);
            state.connect_event_input(input.target, source);
        }
    }
    result.applied_connections.push_back({connection.connection_id, PortKind::event, begin, state.mark().event_connections});
}
} // namespace

std::vector<SampleOutputRoute> resolve_project_sample_output_routes(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    NodeAddress const& address, std::string const& port,
    std::optional<std::size_t> channel)
{
    std::vector<SampleOutputRoute> result;
    ProjectPortMatcher matcher{.name = port, .channel = channel};
    for (auto const& cursor : resolve_node_address(graph, placements, address)) {
        for (auto const& output : sample_outputs_for_cursor(graph, cursor, matcher)) {
            if (output.ambiguous_occurrence) return {};
            for (auto const source : output.routes) append_unique(result, source);
        }
    }
    return result;
}

std::vector<SampleOutputChannelId> resolve_project_sample_output(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    NodeAddress const& address, std::string const& port,
    std::optional<std::size_t> channel)
{
    std::vector<SampleOutputChannelId> result;
    for (auto const& source : resolve_project_sample_output_routes(graph, placements, address, port, channel))
        append_unique(result, source.channel);
    return result;
}

std::vector<SampleInputChannelId> resolve_project_sample_input(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    NodeAddress const& address, std::string const& port,
    std::optional<std::size_t> channel)
{
    std::vector<SampleInputChannelId> result;
    auto const paths = resolve_node_address(graph, placements, address);
    auto collect = [&](std::size_t index) {
        ProjectPortMatcher const matcher{.name = port, .channel = index};
        for (auto const& cursor : paths)
            for (auto const& input : sample_inputs_for_cursor(graph, cursor, matcher))
                append_unique(result, std::get<SampleInputChannelId>(input.target));
    };
    if (channel) collect(*channel);
    else for (std::size_t index = 0; index < maximum_supported_channel_count; ++index) collect(index);
    return result;
}

void validate_project_connection(ProjectConnection const& connection)
{
    validate_connection_impl(connection);
}

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

void GraphConnectionsInteraction::run(
    GraphConnectionsProjectGraphRequest& request)
{
    if (!owner_) {
        throw std::logic_error("GraphConnections interaction is not open");
    }
    if (request.handled) {
        throw std::logic_error("GraphConnections ProjectGraph request was handled more than once");
    }
    if (committed_) throw std::logic_error("GraphConnections transaction commits once");
    EventModuleVisitScope entry(visit_);
    request.handled = true;
    owner_->apply_project_graph_transaction(request);
    committed_ = !request.validate_only && !std::holds_alternative<std::monostate>(request.mutation);
}

std::vector<ProjectConnection> GraphConnectionsInteraction::records() const
{
    if (!owner_) throw std::logic_error("GraphConnections transaction is not open");
    EventModuleVisitScope entry(visit_);
    return owner_->desired_connections();
}

void GraphConnections::handle_project_graph_interaction(
    GraphConnectionsInteractionRequest& request)
{
    if (request.handled) {
        throw std::logic_error("GraphConnections interaction request was handled more than once");
    }
    request.handled = true;
    request.interaction = GraphConnectionsInteraction(
        *this, EventModuleVisit::current<GraphConnections>());
}

void GraphConnections::handle_project_graph_read(GraphConnectionsProjectGraphRead& read) const
{
    read.handled = true;
    read.connections = desired_connections();
}

void GraphConnections::handle_project_graph_transaction(
    GraphConnectionsProjectGraphRequest& request)
{
    if (request.handled)
        throw std::logic_error("GraphConnections ProjectGraph request was handled more than once");
    request.handled = true;
    apply_project_graph_transaction(request);
}

void GraphConnections::apply_project_graph_transaction(
    GraphConnectionsProjectGraphRequest& request)
{
    std::scoped_lock lock(mutex_);
    auto staged = apply_mutation(desired_connections_, request.mutation);
    if (request.validate_only) return;
    if (request.capture_effect) {
        for (auto const& [_, value] : desired_connections_) request.before.push_back(value);
        for (auto const& [_, value] : staged) request.after.push_back(value);
    }
    if (!request.root_builder) {
        desired_connections_ = std::move(staged);
        return;
    }
    if (!request.placements)
        throw std::invalid_argument("GraphConnections transaction requires instance placements");

    // Resolve against an immutable view of exactly the complete root produced
    // by NodeInstances before any project connection is added. Handles in this
    // snapshot are identical to the live builder handles used below.
    auto const snapshot = request.root_builder->finish();

    std::vector<std::string> ordered_ids;
    ordered_ids.reserve(staged.size());
    for (auto const& [id, _] : staged) ordered_ids.push_back(id);
    std::ranges::sort(ordered_ids);

    // Project connections append to these lists. Retain the source-owned
    // prefixes so the graph editor can display them without exposing edits.
    GraphConnectionsBatchResult result{
        .source_sample_count = snapshot.connections.configured_sample_connections().size(),
        .source_event_count = snapshot.connections.configured_event_connections().size(),
    };
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
            // Resolution and compatibility are checked before any mutation;
            // a throw while applying leaves the root graph partial.
            result.failure = GraphConnectionDiagnostic{
                .connection_id = id,
                .message = error.what(),
            };
            break;
        } catch (...) {
            result.failure = GraphConnectionDiagnostic{
                .connection_id = id,
                .message = "unknown project connection application failure",
            };
            break;
        }
    }

    desired_connections_ = std::move(staged);
    request.result = std::move(result);
}
} // namespace iv
