#include <intravenous/graph_jit/connection_plan.h>

#include <intravenous/channel_layout.h>
#include <intravenous/ports.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace iv::graph_jit::detail {
namespace {

std::size_t saturating_add(std::size_t a, std::size_t b) noexcept
{
    auto const max = std::numeric_limits<std::size_t>::max();
    return b > max - a ? max : a + b;
}

std::size_t retained_extent(
    std::size_t source_history,
    std::size_t read_latency,
    std::size_t target_history) noexcept
{
    return saturating_add(
        std::max(source_history, target_history), read_latency);
}

std::expected<std::size_t, std::string> checked_latency_add(
    std::size_t a,
    std::size_t b,
    std::string_view context)
{
    if (b > std::numeric_limits<std::size_t>::max() - a) {
        return std::unexpected(std::string(context) + " overflows size_t");
    }
    return a + b;
}

bool is_internal_bundle(
    NodeBundleHandle bundle,
    NodeBundleHandle boundary) noexcept
{
    return bundle != boundary;
}

std::expected<PlannedConnectionAccess, std::string> connection_access(
    bool source_realtime,
    bool target_realtime,
    std::string_view payload,
    std::size_t connection_index)
{
    if (source_realtime && target_realtime) {
        return PlannedConnectionAccess::realtime_to_realtime;
    }
    if (!source_realtime && !target_realtime) {
        return PlannedConnectionAccess::indexed_to_indexed;
    }
    return std::unexpected(
        "GraphJit " + std::string(payload) + " connection "
        + std::to_string(connection_index)
        + " crosses realtime and indexed access domains; "
          "cross-domain transfer requires an explicit bridge node");
}

bool uses_realtime_storage(PlannedConnectionAccess access) noexcept
{
    return access == PlannedConnectionAccess::realtime_to_realtime;
}

bool has_sequential_source(PlannedConnectionAccess access) noexcept
{
    return access == PlannedConnectionAccess::realtime_to_realtime;
}

std::expected<void, std::string> inventory_nodes(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan)
{
    plan.boundary_bundle = graph.public_ports.boundary_handle();
    // A default-constructed ConfiguredGraph is the shell's semantic empty
    // identity and intentionally has no materialized boundary bundle.
    if (graph.node_bundles.size() == 0) return {};

    std::size_t boundary_count = 0;
    std::size_t bundle = 0;
    std::string error;
    graph.node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            auto const current = bundle++;
            if (!error.empty()) return;

            if (view.kind == ConfiguredNodeBundleKind::boundary) {
                ++boundary_count;
                if (current != plan.boundary_bundle) {
                    error = "connection analysis found a boundary that is not the project boundary";
                }
                return;
            }
            if (view.kind != ConfiguredNodeBundleKind::concrete) {
                error =
                    "connection analysis does not yet support tiled/subgraph bundles";
                return;
            }
            if (!view.ports) {
                error = "concrete bundle has no port metadata during connection analysis";
                return;
            }
            plan.nodes.push_back(PlannedGraphNode{
                .bundle = current,
                .internal_latency_samples = view.internal_latency_samples,
                .maximum_block_size = view.maximum_block_size,
                .sample_input_count = view.ports->sample_input_count(),
                .sample_output_count = view.ports->sample_output_count(),
                .event_input_count = view.ports->event_input_count(),
                .event_output_count = view.ports->event_output_count(),
            });
        });

    if (!error.empty()) return std::unexpected(std::move(error));
    if (boundary_count != 1 || plan.boundary_bundle >= graph.node_bundles.size()) {
        return std::unexpected(
            "connection analysis requires exactly one project boundary");
    }
    return {};
}

void append_dependency(
    ConnectionAnalysisPlan& plan,
    NodeBundleHandle source,
    NodeBundleHandle target,
    PlannedConnectionPayload payload,
    PlannedConnectionAccess access,
    std::size_t connection_index)
{
    if (!is_internal_bundle(source, plan.boundary_bundle)
        || !is_internal_bundle(target, plan.boundary_bundle)) {
        return;
    }
    auto const duplicate = std::ranges::find_if(
        plan.dependencies,
        [&](DependencyEdgePlan const& edge) {
            return edge.source_bundle == source && edge.target_bundle == target
                && edge.payload == payload
                && edge.access == access
                && edge.configured_connection_index == connection_index;
        });
    if (duplicate == plan.dependencies.end()) {
        plan.dependencies.push_back(DependencyEdgePlan{
            .source_bundle = source,
            .target_bundle = target,
            .payload = payload,
            .access = access,
            .configured_connection_index = connection_index,
            .sequential_tick_dependency = has_sequential_source(access),
        });
    }
}

std::expected<void, std::string> inventory_sample_connections(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan)
{
    auto const connections = graph.connections.configured_sample_connections();
    plan.sample_connections.reserve(connections.size());
    for (std::size_t i = 0; i < connections.size(); ++i) {
        auto const& connection = connections[i];
        if (connection.source_channels.empty() || connection.target_channels.empty()) {
            return std::unexpected(
                "configured sample connection has an empty endpoint set");
        }
        auto const first_target = connection.target_channels.front();
        if (!std::ranges::all_of(
                connection.target_channels,
                [&](SampleInputChannelId target) {
                    return target.bundle == first_target.bundle
                        && target.port == first_target.port;
                })) {
            return std::unexpected(
                "configured sample connection target spans multiple input ports");
        }

        SampleConnectionPlan connection_plan{
            .configured_connection_index = i,
            .source_type = connection.source_type,
            .source_channels = connection.source_channels,
            .target_type = connection.target_type,
            .target_channels = connection.target_channels,
            .target_port = NodeBundlePortId{
                first_target.bundle, PortKind::sample, first_target.port},
        };

        try {
            connection_plan.canonical_source_port =
                graph.node_bundles.sample_output_port_for_channels(
                    connection.source_type, connection.source_channels);
            auto const target = graph.node_bundles.resolve_sample_input(
                connection_plan.target_port).config;
            connection_plan.target_layout = target.channel_layout;
            connection_plan.target_history = port_history_or_zero(target);
            if (connection.detach) {
                if (connection.detach->loop_extra_latency == 0)
                    return std::unexpected(
                        "GraphJit sample detach latency must be at least one sample");
                connection_plan.detach = connection.detach;
                connection_plan.detach_initial_value =
                    connection.detach->initial_value_override.value_or(
                        target.neutral_value);
            }
            auto const target_realtime = is_sequential(target.access);

            std::optional<ChannelLayout> canonical_source_layout;
            std::optional<bool> source_realtime;
            connection_plan.source_channel_timings.reserve(
                connection.source_channels.size());
            for (auto const source_channel : connection.source_channels) {
                NodeBundlePortId const source_port{
                    source_channel.bundle, PortKind::sample, source_channel.port};
                auto const source =
                    graph.node_bundles.resolve_sample_output(source_port).config;
                auto const source_history = port_history_or_zero(source);
                auto const source_latency = tick_latency_or_zero(source);
                connection_plan.source_channel_timings.push_back(
                    SampleSourceChannelTimingPlan{
                        .source = source_channel,
                        .source_layout = source.channel_layout,
                        .source_history = source_history,
                        .source_latency = source_latency,
                        .read_latency = source_latency,
                    });
                connection_plan.source_history = std::max(
                    connection_plan.source_history, source_history);
                connection_plan.source_latency = std::max(
                    connection_plan.source_latency, source_latency);
                auto const this_source_realtime = is_tick(source.production);
                if (source_realtime && *source_realtime != this_source_realtime) {
                    return std::unexpected(
                        "sample connection sources mix realtime and indexed access");
                }
                source_realtime = this_source_realtime;
                if (connection_plan.canonical_source_port
                    && *connection_plan.canonical_source_port == source_port) {
                    canonical_source_layout = source.channel_layout;
                }
            }
            connection_plan.canonical_source_layout = canonical_source_layout;
            connection_plan.read_latency = connection_plan.source_latency;
            auto access = connection_access(
                source_realtime.value_or(true), target_realtime, "sample", i);
            if (!access) return std::unexpected(std::move(access.error()));
            connection_plan.access = *access;
            if (!connection.detach) {
                for (auto const source_channel : connection.source_channels) {
                    append_dependency(
                        plan,
                        source_channel.bundle,
                        first_target.bundle,
                        PlannedConnectionPayload::sample,
                        connection_plan.access,
                        i);
                }
            }
            connection_plan.external_boundary =
                first_target.bundle == plan.boundary_bundle
                || std::ranges::any_of(
                    connection.source_channels,
                    [&](SampleOutputChannelId source) {
                        return source.bundle == plan.boundary_bundle;
                    });
            auto const canonical_target_channels =
                graph.node_bundles.sample_input_channels(connection_plan.target_port);
            auto const whole_target =
                connection.target_type == target.channel_layout.channel_type
                && std::ranges::equal(
                    connection.target_channels, canonical_target_channels);
            auto const semantic_target_layout = whole_target
                ? target.channel_layout
                : ChannelLayout{
                    .channel_type = connection.target_type,
                    .sample_layout = SampleStreamLayout::planar,
                };
            connection_plan.requires_conversion =
                !connection_plan.canonical_source_port
                || connection.source_type != connection.target_type
                || !canonical_source_layout
                || *canonical_source_layout != semantic_target_layout;
            if (connection_plan.requires_conversion) {
                // Validate the semantic connection conversion. Partial target
                // contributions convert into their declared target type first;
                // target-port assembly is normalized separately below.
                auto const source_layout = canonical_source_layout.value_or(
                    ChannelLayout{
                        .channel_type = connection.source_type,
                        .sample_layout = SampleStreamLayout::planar,
                    });
                (void)ChannelConversionRegistry::plan(
                    source_layout, semantic_target_layout);
            }
        } catch (std::exception const& e) {
            return std::unexpected(
                "sample connection analysis failed: " + std::string(e.what()));
        }
        plan.sample_connections.push_back(std::move(connection_plan));
    }
    return {};
}

std::expected<void, std::string> normalize_sample_target_projections(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan)
{
    if (plan.sample_connections.empty()) return {};

    std::vector<SampleConnectionPlan> normalized;
    normalized.reserve(plan.sample_connections.size());
    std::vector<bool> consumed(plan.sample_connections.size(), false);

    for (std::size_t first_index = 0;
         first_index < plan.sample_connections.size(); ++first_index) {
        if (consumed[first_index]) continue;
        auto const target_port =
            plan.sample_connections[first_index].target_port;

        std::vector<std::size_t> group_indices;
        for (std::size_t i = first_index;
             i < plan.sample_connections.size(); ++i) {
            if (!consumed[i]
                && plan.sample_connections[i].target_port == target_port) {
                group_indices.push_back(i);
            }
        }

        auto const target = graph.node_bundles.resolve_sample_input(target_port).config;
        auto const canonical_targets =
            graph.node_bundles.sample_input_channels(target_port);
        auto covers_whole_target = [&](SampleConnectionPlan const& connection) {
            return connection.target_type == target.channel_layout.channel_type
                && std::ranges::equal(
                    connection.target_channels, canonical_targets);
        };

        if (group_indices.size() == 1
            && covers_whole_target(plan.sample_connections[first_index])
            && plan.sample_connections[first_index].canonical_source_port) {
            consumed[first_index] = true;
            normalized.push_back(std::move(plan.sample_connections[first_index]));
            continue;
        }

        auto const target_channel_total = canonical_targets.size();
        std::vector<bool> claimed(target_channel_total, false);

        auto const& first = plan.sample_connections[first_index];
        auto access = first.access;
        auto target_history = first.target_history;
        auto detach = first.detach;
        auto detach_initial_value = first.detach_initial_value;
        bool external_boundary = false;
        bool requires_block_materialization = false;

        SampleConnectionPlan merged{
            .configured_connection_index = first.configured_connection_index,
            // A normalized composition can contain heterogeneous semantic
            // source types. source_type is therefore only a conservative
            // aggregate label; projection_contributions below are authoritative.
            .source_type = target.channel_layout.channel_type,
            .target_type = target.channel_layout.channel_type,
            .target_layout = target.channel_layout,
            .target_channels = {
                canonical_targets.begin(), canonical_targets.end()},
            .target_port = target_port,
            .target_history = target_history,
            .access = access,
            .requires_conversion = true,
            .requires_block_materialization = false,
            .external_boundary = false,
            .detach = detach,
            .detach_initial_value = detach_initial_value,
        };

        for (auto const connection_index : group_indices) {
            auto const& connection = plan.sample_connections[connection_index];
            if (connection.target_layout != target.channel_layout
                || connection.target_history != target_history
                || connection.access != access
                || connection.detach != detach) {
                return std::unexpected(
                    "GraphJit sample target-channel projections disagree on target semantics");
            }
            if (connection.source_channels.size()
                    != channel_count(connection.source_type)
                || connection.target_channels.size()
                    != channel_count(connection.target_type)
                || connection.source_channel_timings.size()
                    != connection.source_channels.size()) {
                return std::unexpected(
                    "GraphJit sample target-channel projection has inconsistent semantic channel counts");
            }

            SampleProjectionContributionPlan contribution{
                .source_type = connection.source_type,
                .target_type = connection.target_type,
            };
            contribution.source_channel_indices.reserve(
                connection.source_channel_timings.size());
            contribution.target_channels.reserve(
                connection.target_channels.size());

            for (std::size_t source_channel = 0;
                 source_channel < connection.source_channel_timings.size();
                 ++source_channel) {
                auto const flattened_index =
                    merged.source_channel_timings.size();
                merged.source_channels.push_back(
                    connection.source_channels[source_channel]);
                merged.source_channel_timings.push_back(
                    connection.source_channel_timings[source_channel]);
                contribution.source_channel_indices.push_back(flattened_index);
                merged.source_history = std::max(
                    merged.source_history,
                    connection.source_channel_timings[source_channel]
                        .source_history);
                merged.source_latency = std::max(
                    merged.source_latency,
                    connection.source_channel_timings[source_channel]
                        .source_latency);
            }

            for (auto const target_channel : connection.target_channels) {
                auto const found = std::ranges::find(
                    canonical_targets, target_channel);
                if (found == canonical_targets.end()) {
                    return std::unexpected(
                        "GraphJit sample target-channel projection references a foreign target channel");
                }
                auto const target_ordinal = static_cast<std::size_t>(
                    std::distance(canonical_targets.begin(), found));
                if (claimed[target_ordinal]) {
                    return std::unexpected(
                        "GraphJit sample target channel has more than one source");
                }
                claimed[target_ordinal] = true;
                contribution.target_channels.push_back(target_ordinal);
            }

            // inventory_sample_connections() already validates the configured
            // conversion. Re-check the normalized semantic boundary here so a
            // future producer-layout change cannot silently alter composition.
            try {
                (void)ChannelConversionRegistry::plan(
                    ChannelLayout{
                        .channel_type = contribution.source_type,
                        .sample_layout = SampleStreamLayout::planar,
                    },
                    ChannelLayout{
                        .channel_type = contribution.target_type,
                        .sample_layout = SampleStreamLayout::planar,
                    });
            } catch (std::exception const& e) {
                return std::unexpected(
                    "sample target-channel projection analysis failed: "
                    + std::string(e.what()));
            }

            merged.projection_contributions.push_back(std::move(contribution));
            external_boundary = external_boundary || connection.external_boundary;
            requires_block_materialization = requires_block_materialization
                || connection.requires_block_materialization;
            consumed[connection_index] = true;
        }

        if (!std::ranges::all_of(claimed, [](bool value) { return value; })) {
            return std::unexpected(
                "GraphJit sample target-channel projection does not cover every target channel");
        }

        merged.read_latency = merged.source_latency;
        merged.requires_block_materialization = requires_block_materialization;
        merged.external_boundary = external_boundary;
        // A normalized projection is intentionally kept as a composition even
        // if its flattened channels happen to reconstruct one canonical port;
        // the contribution boundaries carry conversion/projection semantics.
        merged.canonical_source_port.reset();
        merged.canonical_source_layout.reset();
        normalized.push_back(std::move(merged));
    }

    plan.sample_connections = std::move(normalized);
    return {};
}

std::expected<void, std::string> inventory_event_connections(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan)
{
    auto const connections = graph.connections.configured_event_connections();
    plan.event_connections.reserve(connections.size());
    for (std::size_t i = 0; i < connections.size(); ++i) {
        auto const& connection = connections[i];
        if (connection.sources.empty() || connection.targets.empty()) {
            return std::unexpected(
                "configured event connection has an empty endpoint set");
        }
        EventConnectionPlan connection_plan{
            .configured_connection_index = i,
            .source_type = connection.source_type,
            .sources = connection.sources,
            .target_type = connection.target_type,
            .targets = connection.targets,
        };

        try {
            std::optional<bool> source_realtime;
            std::optional<bool> target_realtime;
            for (auto const source_id : connection.sources) {
                NodeBundlePortId const source_port{
                    source_id.bundle, PortKind::event, source_id.port};
                auto const source =
                    graph.node_bundles.resolve_event_output(source_port).config;
                connection_plan.source_history = std::max(
                    connection_plan.source_history,
                    port_history_or_zero(source));
                connection_plan.source_latency = std::max(
                    connection_plan.source_latency,
                    tick_latency_or_zero(source));
                if (!is_valid_event_buffer_rate(source.max_events_per_index)) {
                    return std::unexpected(
                        "event output max_events_per_index must be finite and nonnegative");
                }
                connection_plan.max_events_per_index += source.max_events_per_index;
                if (!is_valid_event_buffer_rate(connection_plan.max_events_per_index)) {
                    return std::unexpected(
                        "event connection aggregate max_events_per_index is not representable");
                }
                auto const this_source_realtime = is_tick(source.production);
                if (source_realtime && *source_realtime != this_source_realtime) {
                    return std::unexpected(
                        "event connection sources mix realtime and indexed access");
                }
                source_realtime = this_source_realtime;
            }
            for (auto const target_id : connection.targets) {
                NodeBundlePortId const target_port{
                    target_id.bundle, PortKind::event, target_id.port};
                auto const target =
                    graph.node_bundles.resolve_event_input(target_port).config;
                connection_plan.target_history = std::max(
                    connection_plan.target_history,
                    port_history_or_zero(target));
                auto const this_target_realtime = is_sequential(target.access);
                if (target_realtime && *target_realtime != this_target_realtime) {
                    return std::unexpected(
                        "event connection targets mix realtime and indexed access");
                }
                target_realtime = this_target_realtime;
            }
            auto access = connection_access(
                source_realtime.value_or(true),
                target_realtime.value_or(true),
                "event",
                i);
            if (!access) return std::unexpected(std::move(access.error()));
            connection_plan.access = *access;
            if (connection.detach) {
                if (connection.detach->loop_extra_latency == 0)
                    return std::unexpected(
                        "GraphJit event detach latency must be at least one sample");
                connection_plan.detach = connection.detach;
            } else {
                for (auto const source_id : connection.sources) {
                    for (auto const target_id : connection.targets) {
                        append_dependency(
                            plan,
                            source_id.bundle,
                            target_id.bundle,
                            PlannedConnectionPayload::event,
                            connection_plan.access,
                            i);
                    }
                }
            }
            connection_plan.external_boundary =
                std::ranges::any_of(
                    connection.sources,
                    [&](EventOutputPortId source) {
                        return source.bundle == plan.boundary_bundle;
                    })
                || std::ranges::any_of(
                    connection.targets,
                    [&](EventInputPortId target) {
                        return target.bundle == plan.boundary_bundle;
                    });
            connection_plan.conversion = EventConversionRegistry::instance().plan(
                connection.source_type, connection.target_type);
            if (!EventConversionRegistry::is_nonexpanding(
                    connection_plan.conversion)) {
                return std::unexpected(
                    "implicit event conversion must be non-expanding");
            }
            connection_plan.requires_conversion =
                connection_plan.conversion.size() != 0
                || connection.sources.size() != 1;
        } catch (std::exception const& e) {
            return std::unexpected(
                "event connection analysis failed: " + std::string(e.what()));
        }
        plan.event_connections.push_back(std::move(connection_plan));
    }
    return {};
}


std::vector<std::vector<std::size_t>> dependency_adjacency(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan,
    bool sequential_only)
{
    std::vector<std::optional<std::size_t>> bundle_to_node(
        graph.node_bundles.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        bundle_to_node[plan.nodes[i].bundle] = i;
    }

    std::vector<std::vector<std::size_t>> outgoing(plan.nodes.size());
    for (auto const& dependency : plan.dependencies) {
        if (sequential_only && !dependency.sequential_tick_dependency) continue;
        if (dependency.source_bundle >= bundle_to_node.size()
            || dependency.target_bundle >= bundle_to_node.size()
            || !bundle_to_node[dependency.source_bundle]
            || !bundle_to_node[dependency.target_bundle]) {
            continue;
        }
        outgoing[*bundle_to_node[dependency.source_bundle]].push_back(
            *bundle_to_node[dependency.target_bundle]);
    }
    for (auto& targets : outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    }
    return outgoing;
}

bool has_path(
    std::vector<std::vector<std::size_t>> const& outgoing,
    std::size_t start,
    std::size_t goal)
{
    if (start == goal) return true;
    std::vector<bool> seen(outgoing.size(), false);
    std::vector<std::size_t> queue{start};
    seen[start] = true;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        for (auto const target : outgoing[queue[i]]) {
            if (target == goal) return true;
            if (!seen[target]) {
                seen[target] = true;
                queue.push_back(target);
            }
        }
    }
    return false;
}

std::expected<void, std::string> validate_explicit_graph_is_acyclic(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan)
{
    auto const outgoing = dependency_adjacency(graph, plan, false);
    std::vector<std::size_t> indegree(outgoing.size(), 0);
    for (auto const& targets : outgoing) {
        for (auto const target : targets) ++indegree[target];
    }
    std::vector<std::size_t> ready;
    for (std::size_t node = 0; node < indegree.size(); ++node) {
        if (indegree[node] == 0) ready.push_back(node);
    }
    std::size_t visited = 0;
    for (std::size_t i = 0; i < ready.size(); ++i) {
        ++visited;
        for (auto const target : outgoing[ready[i]]) {
            if (--indegree[target] == 0) ready.push_back(target);
        }
    }
    if (visited != outgoing.size()) {
        return std::unexpected(
            "GraphJit graph contains an implicit cycle; use detach() to break feedback explicitly");
    }
    return {};
}

std::vector<NodeBundleHandle> unique_source_bundles(
    std::span<SampleOutputChannelId const> sources)
{
    std::vector<NodeBundleHandle> bundles;
    bundles.reserve(sources.size());
    for (auto const source : sources) bundles.push_back(source.bundle);
    std::ranges::sort(bundles);
    bundles.erase(std::unique(bundles.begin(), bundles.end()), bundles.end());
    return bundles;
}

std::vector<NodeBundleHandle> unique_source_bundles(
    std::span<EventOutputPortId const> sources)
{
    std::vector<NodeBundleHandle> bundles;
    bundles.reserve(sources.size());
    for (auto const source : sources) bundles.push_back(source.bundle);
    std::ranges::sort(bundles);
    bundles.erase(std::unique(bundles.begin(), bundles.end()), bundles.end());
    return bundles;
}

std::expected<IndexedPlan, std::string> build_semantic_indexed_plan(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan)
{
    IndexedPlan result;
    result.bundle_to_semantic_node.resize(graph.node_bundles.size());
    result.bundle_to_indexed_node.resize(graph.node_bundles.size());
    result.semantic_nodes.reserve(plan.nodes.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        auto const bundle = plan.nodes[i].bundle;
        result.bundle_to_semantic_node[bundle] = i;
        result.semantic_nodes.push_back(SemanticNodePlan{.bundle = bundle});
    }
    if (plan.nodes.empty()) return result;

    // Semantic cycle membership restores every authored detach edge. Detach
    // changes same-slice execution, not the logical data dependency.
    auto outgoing = dependency_adjacency(graph, plan, false);
    auto append_edge = [&](NodeBundleHandle source_bundle,
                           NodeBundleHandle target_bundle) {
        if (source_bundle >= result.bundle_to_semantic_node.size()
            || target_bundle >= result.bundle_to_semantic_node.size()
            || !result.bundle_to_semantic_node[source_bundle]
            || !result.bundle_to_semantic_node[target_bundle]) {
            return;
        }
        outgoing[*result.bundle_to_semantic_node[source_bundle]].push_back(
            *result.bundle_to_semantic_node[target_bundle]);
    };
    for (auto const& connection : plan.sample_connections) {
        if (!connection.detach) continue;
        for (auto const source : unique_source_bundles(
                 connection.source_channels)) {
            append_edge(source, connection.target_port.node_bundle_handle);
        }
    }
    for (auto const& connection : plan.event_connections) {
        if (!connection.detach) continue;
        for (auto const source : unique_source_bundles(connection.sources)) {
            for (auto const target : connection.targets) {
                append_edge(source, target.bundle);
            }
        }
    }
    for (auto& targets : outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    }

    auto const unvisited = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> index(plan.nodes.size(), unvisited);
    std::vector<std::size_t> lowlink(plan.nodes.size());
    std::vector<std::size_t> component_of(plan.nodes.size(), unvisited);
    std::vector<bool> on_stack(plan.nodes.size(), false);
    std::vector<std::size_t> stack;
    std::vector<std::vector<std::size_t>> components;
    std::size_t next_index = 0;

    auto strongconnect = [&](auto&& self, std::size_t node) -> void {
        index[node] = next_index;
        lowlink[node] = next_index;
        ++next_index;
        stack.push_back(node);
        on_stack[node] = true;
        for (auto const target : outgoing[node]) {
            if (index[target] == unvisited) {
                self(self, target);
                lowlink[node] = std::min(lowlink[node], lowlink[target]);
            } else if (on_stack[target]) {
                lowlink[node] = std::min(lowlink[node], index[target]);
            }
        }
        if (lowlink[node] != index[node]) return;

        auto const component_index = components.size();
        auto& component = components.emplace_back();
        while (true) {
            auto const member = stack.back();
            stack.pop_back();
            on_stack[member] = false;
            component_of[member] = component_index;
            component.push_back(member);
            if (member == node) break;
        }
    };
    for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
        if (index[node] == unvisited) strongconnect(strongconnect, node);
    }

    // Tarjan discovery order is an implementation detail. Retain SCCs in one
    // deterministic condensation-topological order so later scheduling,
    // indexed planning, and optimization can all reuse the same identities.
    std::vector<std::vector<std::size_t>> component_outgoing(components.size());
    std::vector<std::size_t> component_indegree(components.size(), 0);
    for (std::size_t source = 0; source < outgoing.size(); ++source) {
        auto const source_component = component_of[source];
        for (auto const target : outgoing[source]) {
            auto const target_component = component_of[target];
            if (source_component == target_component) continue;
            component_outgoing[source_component].push_back(target_component);
        }
    }
    for (auto& targets : component_outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (auto const target : targets) ++component_indegree[target];
    }
    std::vector<std::size_t> minimum_node(components.size(), unvisited);
    for (std::size_t component = 0; component < components.size(); ++component) {
        std::ranges::sort(components[component]);
        minimum_node[component] = components[component].front();
    }
    std::vector<std::size_t> ready;
    auto insert_ready = [&](std::size_t component) {
        auto const position = std::ranges::lower_bound(
            ready,
            minimum_node[component],
            {},
            [&](std::size_t candidate) { return minimum_node[candidate]; });
        ready.insert(position, component);
    };
    for (std::size_t component = 0; component < components.size(); ++component) {
        if (component_indegree[component] == 0) insert_ready(component);
    }
    std::vector<std::size_t> old_components_in_order;
    old_components_in_order.reserve(components.size());
    while (!ready.empty()) {
        auto const component = ready.front();
        ready.erase(ready.begin());
        old_components_in_order.push_back(component);
        for (auto const target : component_outgoing[component]) {
            if (--component_indegree[target] == 0) insert_ready(target);
        }
    }
    if (old_components_in_order.size() != components.size()) {
        return std::unexpected(
            "GraphJit internal semantic SCC condensation is cyclic");
    }

    std::vector<std::size_t> new_component_of_old(components.size());
    result.semantic_sccs.resize(components.size());
    result.semantic_condensation_order.resize(components.size());
    std::iota(
        result.semantic_condensation_order.begin(),
        result.semantic_condensation_order.end(),
        std::size_t{0});
    for (std::size_t component = 0;
         component < old_components_in_order.size(); ++component) {
        new_component_of_old[old_components_in_order[component]] = component;
    }
    for (std::size_t component = 0;
         component < old_components_in_order.size(); ++component) {
        auto const old_component = old_components_in_order[component];
        auto& retained = result.semantic_sccs[component];
        retained.nodes = components[old_component];
        retained.cyclic = retained.nodes.size() > 1;
        for (auto const node : retained.nodes) {
            result.semantic_nodes[node].scc = component;
            retained.cyclic = retained.cyclic
                || std::ranges::contains(outgoing[node], node);
        }
        for (auto const old_target : component_outgoing[old_component]) {
            retained.outgoing.push_back(new_component_of_old[old_target]);
        }
        std::ranges::sort(retained.outgoing);
    }
    for (std::size_t source = 0;
         source < result.semantic_sccs.size(); ++source) {
        for (auto const target : result.semantic_sccs[source].outgoing) {
            result.semantic_sccs[target].incoming.push_back(source);
        }
    }

    auto validate_edge = [&](NodeBundleHandle source_bundle,
                             NodeBundleHandle target_bundle,
                             std::string_view payload,
                             std::size_t connection_index)
        -> std::expected<void, std::string> {
        if (source_bundle >= result.bundle_to_semantic_node.size()
            || target_bundle >= result.bundle_to_semantic_node.size()
            || !result.bundle_to_semantic_node[source_bundle]
            || !result.bundle_to_semantic_node[target_bundle]) {
            return {};
        }
        auto const source = *result.bundle_to_semantic_node[source_bundle];
        auto const target = *result.bundle_to_semantic_node[target_bundle];
        auto const source_scc = result.semantic_nodes[source].scc;
        if (source_scc != result.semantic_nodes[target].scc) return {};

        auto const& component = result.semantic_sccs[source_scc].nodes;
        std::string participants;
        for (auto const member : component) {
            if (!participants.empty()) participants += ", ";
            participants += std::to_string(result.semantic_nodes[member].bundle);
        }
        return std::unexpected(
            "GraphJit indexed " + std::string(payload) + " connection "
            + std::to_string(connection_index) + " from bundle "
            + std::to_string(source_bundle) + " to bundle "
            + std::to_string(target_bundle)
            + " participates in semantic SCC [" + participants + "]");
    };

    for (auto const& connection : plan.sample_connections) {
        if (connection.access
            == PlannedConnectionAccess::realtime_to_realtime) {
            continue;
        }
        for (auto const source : unique_source_bundles(
                 connection.source_channels)) {
            if (auto valid = validate_edge(
                    source,
                    connection.target_port.node_bundle_handle,
                    "sample",
                    connection.configured_connection_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        }
    }
    for (auto const& connection : plan.event_connections) {
        if (connection.access
            == PlannedConnectionAccess::realtime_to_realtime) {
            continue;
        }
        for (auto const source : unique_source_bundles(connection.sources)) {
            for (auto const target : connection.targets) {
                if (auto valid = validate_edge(
                        source,
                        target.bundle,
                        "event",
                        connection.configured_connection_index);
                    !valid) {
                    return std::unexpected(std::move(valid.error()));
                }
            }
        }
    }
    return result;
}

std::expected<void, std::string> populate_indexed_topology(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& connections,
    IndexedPlan& plan)
{
    auto stable_node_identity = [&](NodeBundleHandle bundle)
        -> std::optional<StableConcreteNodeId> {
        auto const& configured_bundle = graph.node_bundles.bundle(bundle);
        std::optional<StableConcreteNodeId> selected;
        for (auto const handle : configured_bundle.virtual_node_handles()) {
            auto const& record = graph.virtual_nodes.record(handle);
            // Port-only virtual records describe aggregates, not concrete
            // members whose runtime state can be rebound.
            if (record.type_identity != configured_bundle.type_identity()) continue;
            auto const member = std::ranges::find(
                record.node_bundle_handles, bundle);
            if (member == record.node_bundle_handles.end()) continue;
            StableConcreteNodeId candidate{
                .graph = graph.identity.value,
                .virtual_node = record.id,
                .direct_member = static_cast<std::size_t>(
                    member - record.node_bundle_handles.begin()),
            };
            if (!selected
                || std::tie(candidate.virtual_node, candidate.direct_member)
                    < std::tie(
                        selected->virtual_node, selected->direct_member)) {
                selected = std::move(candidate);
            }
        }
        return selected;
    };

    auto has_indexed_port = [&](PlannedGraphNode const& node) {
        auto const bundle = node.bundle;
        for (std::size_t port = 0; port < node.sample_input_count; ++port) {
            if (is_random_access(graph.node_bundles.resolve_sample_input(
                    {bundle, PortKind::sample, port}).config)) return true;
        }
        for (std::size_t port = 0; port < node.sample_output_count; ++port) {
            if (is_tock(graph.node_bundles.resolve_sample_output(
                    {bundle, PortKind::sample, port}).config)) return true;
        }
        for (std::size_t port = 0; port < node.event_input_count; ++port) {
            if (is_random_access(graph.node_bundles.resolve_event_input(
                    {bundle, PortKind::event, port}).config)) return true;
        }
        for (std::size_t port = 0; port < node.event_output_count; ++port) {
            if (is_tock(graph.node_bundles.resolve_event_output(
                    {bundle, PortKind::event, port}).config)) return true;
        }
        return false;
    };

    for (std::size_t semantic_node = 0;
         semantic_node < connections.nodes.size(); ++semantic_node) {
        auto const& node = connections.nodes[semantic_node];
        if (!has_indexed_port(node)) continue;
        auto const indexed_node = plan.nodes.size();
        plan.bundle_to_indexed_node[node.bundle] = indexed_node;
        plan.semantic_nodes[semantic_node].indexed_node = indexed_node;
        plan.nodes.push_back(IndexedNodePlan{
            .bundle = node.bundle,
            .semantic_node = semantic_node,
            .semantic_scc = plan.semantic_nodes[semantic_node].scc,
            .stable_identity = stable_node_identity(node.bundle),
        });
    }

    std::map<NodeBundlePortId, IndexedEndpointOrdinal, NodeBundlePortIdLess>
        endpoint_by_port;
    auto append_endpoint = [&](IndexedNodeOrdinal node,
                               NodeBundlePortId port,
                               IndexedEndpointDirection direction,
                               std::string name,
                               std::optional<OutputRetention> retention,
                               ChannelLayout sample_layout,
                               EventTypeId event_type,
                               double max_events_per_index) {
        auto& node_plan = plan.nodes[node];
        auto const endpoint = plan.endpoints.size();
        auto& node_accumulators = node_plan.accumulators;
        IndexedAccumulatorSlotPlan slots;
        if (direction == IndexedEndpointDirection::input) {
            if (node_accumulators.input_change_count == 0) {
                node_accumulators.input_change_begin =
                    plan.accumulators.input_change_count;
                node_accumulators.input_requirement_begin =
                    plan.accumulators.input_requirement_count;
            }
            slots.input_change = plan.accumulators.input_change_count++;
            slots.input_requirement =
                plan.accumulators.input_requirement_count++;
            ++node_accumulators.input_change_count;
            ++node_accumulators.input_requirement_count;
            node_plan.inputs.push_back(endpoint);
        } else {
            if (node_accumulators.output_change_count == 0) {
                node_accumulators.output_change_begin =
                    plan.accumulators.output_change_count;
                node_accumulators.output_requirement_begin =
                    plan.accumulators.output_requirement_count;
            }
            slots.output_change = plan.accumulators.output_change_count++;
            slots.output_requirement =
                plan.accumulators.output_requirement_count++;
            ++node_accumulators.output_change_count;
            ++node_accumulators.output_requirement_count;
            node_plan.outputs.push_back(endpoint);
            plan.requestable_outputs.push_back(endpoint);
        }
        std::optional<StableIndexedOutputId> stable_output;
        if (direction == IndexedEndpointDirection::output
            && node_plan.stable_identity) {
            stable_output = StableIndexedOutputId{
                .node = *node_plan.stable_identity,
                .kind = port.port_kind,
                .port_name = name,
                .port_ordinal = port.port_ordinal,
            };
        }
        plan.endpoints.push_back(IndexedEndpointPlan{
            .node = node,
            .configured_port = port,
            .kind = port.port_kind,
            .direction = direction,
            .name = std::move(name),
            .retention = retention,
            .stable_identity = std::move(stable_output),
            .sample_layout = sample_layout,
            .event_type = event_type,
            .max_events_per_index = max_events_per_index,
            .accumulators = slots,
        });
        endpoint_by_port.emplace(port, endpoint);
    };

    for (IndexedNodeOrdinal indexed_node = 0;
         indexed_node < plan.nodes.size(); ++indexed_node) {
        auto const semantic_node = plan.nodes[indexed_node].semantic_node;
        auto const& node = connections.nodes[semantic_node];
        auto const bundle = node.bundle;
        for (std::size_t port = 0; port < node.sample_input_count; ++port) {
            NodeBundlePortId const id{bundle, PortKind::sample, port};
            auto const config = graph.node_bundles.resolve_sample_input(id).config;
            if (!is_random_access(config)) continue;
            append_endpoint(
                indexed_node, id, IndexedEndpointDirection::input,
                config.name, std::nullopt, config.channel_layout,
                EventTypeId::empty, 0.0);
        }
        for (std::size_t port = 0; port < node.event_input_count; ++port) {
            NodeBundlePortId const id{bundle, PortKind::event, port};
            auto const config = graph.node_bundles.resolve_event_input(id).config;
            if (!is_random_access(config)) continue;
            append_endpoint(
                indexed_node, id, IndexedEndpointDirection::input,
                config.name, std::nullopt, {}, config.type, 0.0);
        }
        for (std::size_t port = 0; port < node.sample_output_count; ++port) {
            NodeBundlePortId const id{bundle, PortKind::sample, port};
            auto const config = graph.node_bundles.resolve_sample_output(id).config;
            if (!is_tock(config)) continue;
            append_endpoint(
                indexed_node, id, IndexedEndpointDirection::output,
                config.name, config.retention,
                config.channel_layout, EventTypeId::empty, 0.0);
        }
        for (std::size_t port = 0; port < node.event_output_count; ++port) {
            NodeBundlePortId const id{bundle, PortKind::event, port};
            auto const config = graph.node_bundles.resolve_event_output(id).config;
            if (!is_tock(config)) continue;
            append_endpoint(
                indexed_node, id, IndexedEndpointDirection::output,
                config.name, config.retention,
                {}, config.type, config.max_events_per_index);
        }
    }

    auto endpoint_for = [&](NodeBundlePortId port)
        -> std::expected<IndexedEndpointOrdinal, std::string> {
        auto const found = endpoint_by_port.find(port);
        if (found == endpoint_by_port.end()) {
            return std::unexpected(
                "GraphJit indexed connection references a non-indexed endpoint");
        }
        return found->second;
    };
    auto append_unique_port = [](std::vector<NodeBundlePortId>& ports,
                                 NodeBundlePortId port) {
        if (!std::ranges::contains(ports, port)) ports.push_back(port);
    };
    auto append_connection = [&](IndexedConnectionPlan connection) {
        auto const ordinal = plan.connections.size();
        for (auto const endpoint : connection.source_endpoints) {
            auto& outgoing_connections =
                plan.endpoints[endpoint].outgoing_connections;
            if (!std::ranges::contains(outgoing_connections, ordinal)) {
                outgoing_connections.push_back(ordinal);
            }
        }
        for (auto const endpoint : connection.target_endpoints) {
            auto& incoming_connections =
                plan.endpoints[endpoint].incoming_connections;
            if (!std::ranges::contains(incoming_connections, ordinal)) {
                incoming_connections.push_back(ordinal);
            }
        }
        plan.connections.push_back(std::move(connection));
    };

    for (auto const& connection : connections.sample_connections) {
        if (connection.access
            == PlannedConnectionAccess::realtime_to_realtime) continue;
        IndexedConnectionPlan indexed{
            .configured_connection_index = connection.configured_connection_index,
            .kind = PortKind::sample,
            .sample_source_type = connection.source_type,
            .sample_target_type = connection.target_type,
            .sample_source_channels = connection.source_channels,
            .sample_target_channels = connection.target_channels,
            .requires_conversion = connection.requires_conversion,
        };
        for (auto const channel : connection.source_channels) {
            NodeBundlePortId const port{
                channel.bundle, PortKind::sample, channel.port};
            if (std::ranges::contains(indexed.source_ports, port)) continue;
            indexed.source_ports.push_back(port);
            auto endpoint = endpoint_for(port);
            if (!endpoint) return std::unexpected(std::move(endpoint.error()));
            indexed.source_endpoints.push_back(*endpoint);
        }
        indexed.target_ports.push_back(connection.target_port);
        auto target_endpoint = endpoint_for(connection.target_port);
        if (!target_endpoint) {
            return std::unexpected(std::move(target_endpoint.error()));
        }
        indexed.target_endpoints.push_back(*target_endpoint);
        for (auto const& projection : connection.projection_contributions) {
            indexed.sample_projections.push_back(IndexedSampleProjectionPlan{
                .source_type = projection.source_type,
                .source_channel_indices = projection.source_channel_indices,
                .target_type = projection.target_type,
                .target_channels = projection.target_channels,
            });
        }
        append_connection(std::move(indexed));
    }
    for (auto const& connection : connections.event_connections) {
        if (connection.access
            == PlannedConnectionAccess::realtime_to_realtime) continue;
        IndexedConnectionPlan indexed{
            .configured_connection_index = connection.configured_connection_index,
            .kind = PortKind::event,
            .event_source_type = connection.source_type,
            .event_target_type = connection.target_type,
            .event_conversion = connection.conversion,
            .requires_conversion = connection.requires_conversion,
        };
        for (auto const source : connection.sources) {
            append_unique_port(indexed.source_ports, {
                source.bundle, PortKind::event, source.port});
        }
        for (auto const port : indexed.source_ports) {
            auto endpoint = endpoint_for(port);
            if (!endpoint) return std::unexpected(std::move(endpoint.error()));
            indexed.source_endpoints.push_back(*endpoint);
        }
        for (auto const target : connection.targets) {
            append_unique_port(indexed.target_ports, {
                target.bundle, PortKind::event, target.port});
        }
        for (auto const port : indexed.target_ports) {
            auto endpoint = endpoint_for(port);
            if (!endpoint) {
                return std::unexpected(std::move(endpoint.error()));
            }
            indexed.target_endpoints.push_back(*endpoint);
        }
        append_connection(std::move(indexed));
    }

    std::vector<std::vector<IndexedNodeOrdinal>> indexed_outgoing(
        plan.nodes.size());
    std::vector<IndexedNodeOrdinal> parent(plan.nodes.size());
    std::iota(parent.begin(), parent.end(), IndexedNodeOrdinal{0});
    auto find_root = [&](auto&& self, IndexedNodeOrdinal node)
        -> IndexedNodeOrdinal {
        if (parent[node] == node) return node;
        parent[node] = self(self, parent[node]);
        return parent[node];
    };
    auto join = [&](IndexedNodeOrdinal lhs, IndexedNodeOrdinal rhs) {
        lhs = find_root(find_root, lhs);
        rhs = find_root(find_root, rhs);
        if (lhs == rhs) return;
        if (rhs < lhs) std::swap(lhs, rhs);
        parent[rhs] = lhs;
    };
    for (auto const& connection : plan.connections) {
        std::vector<IndexedNodeOrdinal> participating_nodes;
        for (auto const endpoint : connection.source_endpoints) {
            auto const node = plan.endpoints[endpoint].node;
            if (!std::ranges::contains(participating_nodes, node)) {
                participating_nodes.push_back(node);
            }
        }
        for (auto const endpoint : connection.target_endpoints) {
            auto const node = plan.endpoints[endpoint].node;
            if (!std::ranges::contains(participating_nodes, node)) {
                participating_nodes.push_back(node);
            }
        }
        for (std::size_t i = 1; i < participating_nodes.size(); ++i) {
            join(participating_nodes.front(), participating_nodes[i]);
        }
        for (auto const source_endpoint : connection.source_endpoints) {
            auto const source = plan.endpoints[source_endpoint].node;
            for (auto const target_endpoint : connection.target_endpoints) {
                auto const target = plan.endpoints[target_endpoint].node;
                indexed_outgoing[source].push_back(target);
            }
        }
    }

    std::vector<std::size_t> indegree(plan.nodes.size(), 0);
    for (auto& targets : indexed_outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (auto const target : targets) ++indegree[target];
    }
    std::vector<IndexedNodeOrdinal> ready_nodes;
    auto insert_ready_node = [&](IndexedNodeOrdinal node) {
        ready_nodes.insert(std::ranges::lower_bound(ready_nodes, node), node);
    };
    for (IndexedNodeOrdinal node = 0; node < plan.nodes.size(); ++node) {
        if (indegree[node] == 0) insert_ready_node(node);
    }
    std::vector<IndexedNodeOrdinal> indexed_order;
    indexed_order.reserve(plan.nodes.size());
    while (!ready_nodes.empty()) {
        auto const node = ready_nodes.front();
        ready_nodes.erase(ready_nodes.begin());
        indexed_order.push_back(node);
        for (auto const target : indexed_outgoing[node]) {
            if (--indegree[target] == 0) insert_ready_node(target);
        }
    }
    if (indexed_order.size() != plan.nodes.size()) {
        return std::unexpected(
            "GraphJit indexed dependency graph is cyclic after semantic SCC validation");
    }

    std::map<IndexedNodeOrdinal, std::size_t> component_by_root;
    for (auto const node : indexed_order) {
        auto const root = find_root(find_root, node);
        auto [entry, inserted] = component_by_root.emplace(
            root, plan.components.size());
        if (inserted) {
            plan.component_order.push_back(entry->second);
            plan.components.emplace_back();
        }
        auto& component = plan.components[entry->second];
        component.nodes.push_back(node);
        component.forward_order.push_back(node);
        component.tock_order.push_back(node);
        auto const scc = plan.nodes[node].semantic_scc;
        if (!std::ranges::contains(component.semantic_scc_order, scc)) {
            component.semantic_scc_order.push_back(scc);
        }
    }
    for (auto& component : plan.components) {
        component.reverse_order = component.forward_order;
        std::ranges::reverse(component.reverse_order);
    }
    for (IndexedConnectionOrdinal connection = 0;
         connection < plan.connections.size(); ++connection) {
        auto const& planned = plan.connections[connection];
        if (planned.source_endpoints.empty()) continue;
        auto const node = plan.endpoints[planned.source_endpoints.front()].node;
        auto const root = find_root(find_root, node);
        plan.components[component_by_root.at(root)]
            .connections.push_back(connection);
    }

    return {};
}

std::expected<void, std::string> validate_detached_connections(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan)
{
    auto const semantic_outgoing = dependency_adjacency(graph, plan, false);
    std::vector<std::optional<std::size_t>> bundle_to_node(
        graph.node_bundles.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i)
        bundle_to_node[plan.nodes[i].bundle] = i;

    auto validate = [&](std::span<NodeBundleHandle const> sources,
                        std::span<NodeBundleHandle const> targets,
                        std::string_view kind,
                        std::size_t configured_connection_index)
        -> std::expected<void, std::string> {
        if (sources.empty() || targets.empty())
            return std::unexpected(
                "GraphJit " + std::string(kind)
                + " detach has an empty endpoint set");
        for (auto const target : targets) {
            if (target >= bundle_to_node.size() || !bundle_to_node[target])
                return std::unexpected(
                    "GraphJit detached connection must target a concrete graph node");
            bool closes_semantic_cycle = false;
            for (auto const source : sources) {
                if (source >= bundle_to_node.size() || !bundle_to_node[source])
                    continue;
                closes_semantic_cycle = closes_semantic_cycle
                    || has_path(
                        semantic_outgoing,
                        *bundle_to_node[target],
                        *bundle_to_node[source]);
            }
            if (!closes_semantic_cycle) {
                return std::unexpected(
                    "GraphJit " + std::string(kind) + " detach on connection "
                    + std::to_string(configured_connection_index)
                    + " breaks an acyclic dependency");
            }
        }
        return {};
    };

    for (auto const& connection : plan.sample_connections) {
        if (!connection.detach) continue;
        auto sources = unique_source_bundles(connection.source_channels);
        std::array<NodeBundleHandle, 1> targets{
            connection.target_port.node_bundle_handle};
        if (auto result = validate(
                sources, targets, "sample",
                connection.configured_connection_index);
            !result) return result;
    }
    for (auto const& connection : plan.event_connections) {
        if (!connection.detach) continue;
        auto sources = unique_source_bundles(connection.sources);
        std::vector<NodeBundleHandle> targets;
        targets.reserve(connection.targets.size());
        for (auto const target : connection.targets)
            targets.push_back(target.bundle);
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        if (auto result = validate(
                sources, targets, "event",
                connection.configured_connection_index);
            !result) return result;
    }
    return {};
}

std::size_t floor_power_of_two(std::size_t value) noexcept
{
    std::size_t result = 1;
    while (result <= value / 2) result *= 2;
    return result;
}

std::expected<SchedulePlan, std::string> build_schedule(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan)
{
    SchedulePlan schedule;
    schedule.bundle_to_region.resize(graph.node_bundles.size());
    schedule.bundle_execution_position.resize(graph.node_bundles.size());
    if (plan.nodes.empty()) return schedule;

    // Ordinary tick dependencies remain acyclic. Semantic SCC membership and
    // condensation order were already computed with every authored detach
    // restored while building IndexedPlan; schedule formation reuses those
    // facts and only derives the within-SCC same-slice order here.
    auto const explicit_outgoing = dependency_adjacency(graph, plan, true);
    if (plan.indexed.semantic_nodes.size() != plan.nodes.size()
        || plan.indexed.semantic_sccs.empty()) {
        return std::unexpected(
            "GraphJit schedule is missing retained semantic SCC facts");
    }
    schedule.regions.reserve(plan.indexed.semantic_sccs.size());
    std::vector<std::size_t> node_to_region(plan.nodes.size(), 0);
    for (auto const& semantic_scc : plan.indexed.semantic_sccs) {
        auto const& component = semantic_scc.nodes;
        auto const region_index = schedule.regions.size();
        SccRegionPlan region;
        region.maximum_block_size = std::numeric_limits<std::size_t>::max();
        for (auto const node : component) {
            node_to_region[node] = region_index;
            region.nodes.push_back(plan.nodes[node].bundle);
            region.maximum_block_size = std::min(
                region.maximum_block_size,
                plan.nodes[node].maximum_block_size);
        }
        region.cyclic = semantic_scc.cyclic;

        // A detached dependency is a storage/temporal dependency, not a
        // same-slice tick dependency. Topologically order each region using
        // only ordinary sequential dependencies.
        std::vector<std::size_t> local_indegree(plan.nodes.size(), 0);
        for (auto const node : component) {
            for (auto const target : explicit_outgoing[node]) {
                if (std::ranges::find(component, target) != component.end()) {
                    ++local_indegree[target];
                }
            }
        }
        std::vector<std::size_t> ready;
        for (auto const node : component) {
            if (local_indegree[node] == 0) ready.push_back(node);
        }
        auto by_bundle = [&](std::size_t a, std::size_t b) {
            return plan.nodes[a].bundle < plan.nodes[b].bundle;
        };
        std::ranges::sort(ready, by_bundle);
        while (!ready.empty()) {
            auto const node = ready.front();
            ready.erase(ready.begin());
            region.execution_order.push_back(plan.nodes[node].bundle);
            for (auto const target : explicit_outgoing[node]) {
                if (std::ranges::find(component, target) == component.end()) continue;
                if (--local_indegree[target] == 0) {
                    auto const insertion = std::lower_bound(
                        ready.begin(), ready.end(), target, by_bundle);
                    ready.insert(insertion, target);
                }
            }
        }
        if (region.execution_order.size() != component.size()) {
            return std::unexpected(
                "GraphJit SCC remains cyclic after detach dependencies are removed");
        }
        schedule.regions.push_back(std::move(region));
    }

    for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
        schedule.bundle_to_region[plan.nodes[node].bundle] = node_to_region[node];
    }

    // Schedule condensation is a semantically distinct adjacency: indexed
    // dependencies must not pretend that tick_block produces their values.
    // Reuse the retained SCC partition, then order those SCCs using only
    // ordinary sequential tick dependencies.
    std::vector<std::vector<std::size_t>> region_outgoing(
        schedule.regions.size());
    std::vector<std::size_t> region_indegree(schedule.regions.size(), 0);
    for (std::size_t source = 0; source < explicit_outgoing.size(); ++source) {
        for (auto const target : explicit_outgoing[source]) {
            auto const source_region = node_to_region[source];
            auto const target_region = node_to_region[target];
            if (source_region == target_region) continue;
            region_outgoing[source_region].push_back(target_region);
        }
    }
    for (auto& targets : region_outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (auto const target : targets) ++region_indegree[target];
    }
    auto region_key = [&](std::size_t region) {
        return schedule.regions[region].nodes.front();
    };
    std::vector<std::size_t> ready_regions;
    for (std::size_t region = 0; region < schedule.regions.size(); ++region) {
        if (region_indegree[region] == 0) ready_regions.push_back(region);
    }
    std::ranges::sort(ready_regions, {}, region_key);
    while (!ready_regions.empty()) {
        auto const region = ready_regions.front();
        ready_regions.erase(ready_regions.begin());
        schedule.region_order.push_back(region);
        for (auto const target : region_outgoing[region]) {
            if (--region_indegree[target] == 0) {
                auto const insertion = std::lower_bound(
                    ready_regions.begin(), ready_regions.end(), target,
                    [&](std::size_t lhs, std::size_t rhs) {
                        return region_key(lhs) < region_key(rhs);
                    });
                ready_regions.insert(insertion, target);
            }
        }
    }
    if (schedule.region_order.size() != schedule.regions.size()) {
        return std::unexpected(
            "GraphJit sequential SCC condensation graph is cyclic");
    }

    std::size_t execution_position = 0;
    for (auto const region_index : schedule.region_order) {
        for (auto const bundle : schedule.regions[region_index].execution_order)
            schedule.bundle_execution_position[bundle] = execution_position++;
    }
    return schedule;
}

std::expected<void, std::string> bind_detach_regions(
    ConnectionAnalysisPlan& plan)
{
    auto bind = [&](auto& connection, NodeBundleHandle target_bundle)
        -> std::expected<void, std::string> {
        if (!connection.detach) return {};
        if (target_bundle >= plan.schedule.bundle_to_region.size()
            || !plan.schedule.bundle_to_region[target_bundle]) {
            return std::unexpected(
                "GraphJit detach does not map to a concrete execution region");
        }
        auto const region_index = *plan.schedule.bundle_to_region[target_bundle];
        if (region_index >= plan.schedule.regions.size()
            || !plan.schedule.regions[region_index].cyclic) {
            return std::unexpected(
                "GraphJit validated detach did not form an execution SCC");
        }
        connection.detach_region = region_index;
        auto& region = plan.schedule.regions[region_index];
        region.maximum_block_size = std::min(
            region.maximum_block_size,
            floor_power_of_two(connection.detach->loop_extra_latency));
        return {};
    };

    for (auto& connection : plan.sample_connections) {
        if (auto bound = bind(
                connection, connection.target_port.node_bundle_handle);
            !bound) return std::unexpected(std::move(bound.error()));
    }
    for (auto& connection : plan.event_connections) {
        if (!connection.detach) continue;
        if (connection.targets.empty())
            return std::unexpected("GraphJit detached event connection has no target");
        if (auto bound = bind(connection, connection.targets.front().bundle);
            !bound) return std::unexpected(std::move(bound.error()));
    }
    for (auto& region : plan.schedule.regions) {
        region.scc_feedback_latency = region.cyclic
            ? region.maximum_block_size
            : 0;
    }
    return {};
}

std::size_t effective_block_size(
    ConnectionAnalysisPlan const& plan,
    NodeBundleHandle bundle,
    std::size_t kernel_block_size)
{
    if (bundle == plan.boundary_bundle) return kernel_block_size;
    if (bundle >= plan.schedule.bundle_to_region.size()
        || !plan.schedule.bundle_to_region[bundle]) {
        return kernel_block_size;
    }
    auto const region = *plan.schedule.bundle_to_region[bundle];
    if (region >= plan.schedule.regions.size()) return kernel_block_size;
    return std::min(
        kernel_block_size, plan.schedule.regions[region].maximum_block_size);
}

std::expected<void, std::string> plan_sample_latency_compensation(
    ConnectionAnalysisPlan& plan)
{
    // The feed-forward latency carried by a concrete node's output values,
    // before the authored latency of any particular output port. Once all
    // inputs of a node are aligned, every output shares the same upstream
    // path latency plus the node's intrinsic internal latency.
    std::vector<std::optional<std::size_t>> node_output_latency(
        plan.schedule.bundle_to_region.size());

    auto planned_node = [&](NodeBundleHandle bundle) -> PlannedGraphNode const* {
        auto const found = std::ranges::find_if(
            plan.nodes,
            [&](PlannedGraphNode const& node) { return node.bundle == bundle; });
        return found == plan.nodes.end() ? nullptr : &*found;
    };

    // Keep authored output latency as the default for connections not covered
    // by this feed-forward realtime pass (indexed access, boundaries, and
    // cyclic regions whose feedback latency belongs to point 12).
    for (auto& connection : plan.sample_connections) {
        connection.read_latency = connection.source_latency;
        for (auto& channel : connection.source_channel_timings) {
            channel.read_latency = channel.source_latency;
        }
    }

    for (auto const region_index : plan.schedule.region_order) {
        if (region_index >= plan.schedule.regions.size()) {
            return std::unexpected(
                "sample latency compensation references an invalid schedule region");
        }
        auto const& region = plan.schedule.regions[region_index];

        if (region.cyclic) {
            // Feedback-aware SCC latency is deliberately deferred. Mark a
            // conservative provisional base so analysis of downstream regions
            // remains total; GraphJit's current lowering capability gate still
            // rejects feedback sample storage before execution.
            for (auto const bundle : region.execution_order) {
                auto const* node = planned_node(bundle);
                if (!node || bundle >= node_output_latency.size()) continue;
                node_output_latency[bundle] = node->internal_latency_samples;
            }
            continue;
        }

        for (auto const bundle : region.execution_order) {
            auto const* node = planned_node(bundle);
            if (!node || bundle >= node_output_latency.size()) {
                return std::unexpected(
                    "sample latency compensation lost a scheduled concrete node");
            }

            struct IncomingPath {
                std::size_t connection_index = 0;
                std::size_t source_channel_index = 0;
                std::size_t arrival_latency = 0;
            };
            std::vector<IncomingPath> incoming;
            std::size_t aligned_input_latency = 0;

            for (std::size_t connection_index = 0;
                 connection_index < plan.sample_connections.size();
                 ++connection_index) {
                auto const& connection = plan.sample_connections[connection_index];
                if (connection.access
                        != PlannedConnectionAccess::realtime_to_realtime
                    || connection.detach
                    || connection.target_port.node_bundle_handle != bundle) {
                    continue;
                }
                if (connection.source_channel_timings.size()
                    != connection.source_channels.size()) {
                    return std::unexpected(
                        "sample latency compensation lost source-channel timing metadata");
                }

                for (std::size_t source_channel_index = 0;
                     source_channel_index < connection.source_channel_timings.size();
                     ++source_channel_index) {
                    auto const& channel =
                        connection.source_channel_timings[source_channel_index];
                    std::size_t upstream_latency = 0;
                    if (channel.source.bundle != plan.boundary_bundle) {
                        if (channel.source.bundle >= node_output_latency.size()
                            || !node_output_latency[channel.source.bundle]) {
                            return std::unexpected(
                                "sample latency compensation encountered a source before its producer");
                        }
                        upstream_latency = *node_output_latency[channel.source.bundle];
                    }

                    auto arrival = checked_latency_add(
                        upstream_latency,
                        channel.source_latency,
                        "sample channel path latency");
                    if (!arrival) {
                        return std::unexpected(std::move(arrival.error()));
                    }
                    aligned_input_latency = std::max(
                        aligned_input_latency, *arrival);
                    incoming.push_back(IncomingPath{
                        .connection_index = connection_index,
                        .source_channel_index = source_channel_index,
                        .arrival_latency = *arrival,
                    });
                }
            }

            for (auto const& path : incoming) {
                auto& connection = plan.sample_connections[path.connection_index];
                auto& channel =
                    connection.source_channel_timings[path.source_channel_index];
                auto const compensation =
                    aligned_input_latency - path.arrival_latency;
                auto read_latency = checked_latency_add(
                    channel.source_latency,
                    compensation,
                    "sample compensated channel read latency");
                if (!read_latency) {
                    return std::unexpected(std::move(read_latency.error()));
                }
                channel.read_latency = *read_latency;
            }

            // Whole-port sources necessarily share one producer path and one
            // authored port latency, so every channel has the same effective
            // read latency. For composed sources retain a conservative scalar
            // maximum for the existing storage planner; explicit channel
            // composition will consume source_channel_timings directly.
            for (auto& connection : plan.sample_connections) {
                if (connection.target_port.node_bundle_handle != bundle
                    || connection.source_channel_timings.empty()) {
                    continue;
                }
                connection.read_latency = 0;
                for (auto const& channel : connection.source_channel_timings) {
                    connection.read_latency = std::max(
                        connection.read_latency, channel.read_latency);
                }
            }

            auto output_latency = checked_latency_add(
                aligned_input_latency,
                node->internal_latency_samples,
                "sample node output path latency");
            if (!output_latency) {
                return std::unexpected(std::move(output_latency.error()));
            }
            node_output_latency[bundle] = *output_latency;
        }
    }

    return {};
}

std::size_t scheduled_execution_count(ConnectionAnalysisPlan const& plan)
{
    std::size_t result = 0;
    for (auto const& position : plan.schedule.bundle_execution_position) {
        if (position) result = std::max(result, *position + 1);
    }
    return result;
}

ConnectionLiveIntervalPlan live_interval_for_sample_group(
    ConnectionAnalysisPlan const& plan,
    SampleProducerGroupPlan const& group)
{
    auto const execution_count = scheduled_execution_count(plan);
    ConnectionLiveIntervalPlan live{
        .begin = execution_count,
        .end = 0,
    };
    bool saw_endpoint = false;

    if (group.source_port) {
        saw_endpoint = true;
        auto const source_bundle = group.source_port->node_bundle_handle;
        if (source_bundle == plan.boundary_bundle) {
            live.begin = 0;
        } else if (source_bundle < plan.schedule.bundle_execution_position.size()
            && plan.schedule.bundle_execution_position[source_bundle]) {
            live.begin = *plan.schedule.bundle_execution_position[source_bundle];
        }
    }

    for (auto const connection_index : group.connection_indices) {
        auto const& connection = plan.sample_connections[connection_index];
        if (!uses_realtime_storage(connection.access)) continue;
        auto const target = connection.target_port.node_bundle_handle;
        saw_endpoint = true;
        if (target == plan.boundary_bundle) {
            live.end = execution_count;
        } else if (target < plan.schedule.bundle_execution_position.size()
            && plan.schedule.bundle_execution_position[target]) {
            live.end = std::max(
                live.end,
                *plan.schedule.bundle_execution_position[target]);
        }
        live.crosses_kernel_invocations =
            live.crosses_kernel_invocations || connection.detach;
    }
    if (!saw_endpoint) live.begin = 0;
    if (live.begin == execution_count && execution_count != 0) live.begin = 0;
    if (live.end < live.begin) live.end = live.begin;
    return live;
}

ConnectionLiveIntervalPlan live_interval_for_event_group(
    ConnectionAnalysisPlan const& plan,
    EventProducerGroupPlan const& group)
{
    auto const execution_count = scheduled_execution_count(plan);
    ConnectionLiveIntervalPlan live{
        .begin = execution_count,
        .end = 0,
    };
    bool saw_endpoint = false;
    for (auto const connection_index : group.connection_indices) {
        auto const& connection = plan.event_connections[connection_index];
        if (!uses_realtime_storage(connection.access)) continue;
        for (auto const source : connection.sources) {
            saw_endpoint = true;
            if (source.bundle == plan.boundary_bundle) {
                live.begin = 0;
            } else if (source.bundle < plan.schedule.bundle_execution_position.size()
                && plan.schedule.bundle_execution_position[source.bundle]) {
                live.begin = std::min(
                    live.begin,
                    *plan.schedule.bundle_execution_position[source.bundle]);
            }
        }
        for (auto const target : connection.targets) {
            saw_endpoint = true;
            if (target.bundle == plan.boundary_bundle) {
                live.end = execution_count;
            } else if (target.bundle < plan.schedule.bundle_execution_position.size()
                && plan.schedule.bundle_execution_position[target.bundle]) {
                live.end = std::max(
                    live.end,
                    *plan.schedule.bundle_execution_position[target.bundle]);
            }
        }
        live.crosses_kernel_invocations =
            live.crosses_kernel_invocations || connection.detach;
    }
    if (!saw_endpoint) live.begin = 0;
    if (live.begin == execution_count && execution_count != 0) live.begin = 0;
    if (live.end < live.begin) live.end = live.begin;
    return live;
}

void plan_sample_groups(
    ConnectionAnalysisPlan& plan,
    std::size_t kernel_block_size,
    RealtimeStorageCostModel const& cost_model)
{
    auto source_port_for = [](SampleOutputChannelId channel) {
        return NodeBundlePortId{
            channel.bundle, PortKind::sample, channel.port};
    };

    for (std::size_t i = 0; i < plan.sample_connections.size(); ++i) {
        auto& connection = plan.sample_connections[i];
        if (connection.source_channel_timings.size()
            != connection.source_channels.size()) {
            continue;
        }

        if (uses_realtime_storage(connection.access)) {
            auto const target_block = effective_block_size(
                plan,
                connection.target_port.node_bundle_handle,
                kernel_block_size);
            for (auto const& channel : connection.source_channel_timings) {
                if (effective_block_size(
                        plan, channel.source.bundle, kernel_block_size)
                    != target_block) {
                    connection.requires_block_materialization = true;
                    break;
                }
            }
        }

        for (auto const& channel : connection.source_channel_timings) {
            auto const source_port = source_port_for(channel.source);
            auto group = std::ranges::find_if(
                plan.sample_producer_groups,
                [&](SampleProducerGroupPlan const& candidate) {
                    return candidate.source_port
                        && *candidate.source_port == source_port;
                });
            if (group == plan.sample_producer_groups.end()) {
                std::vector<SampleOutputChannelId> source_channels;
                auto const channel_total = channel_count(channel.source_layout);
                source_channels.reserve(channel_total);
                for (std::size_t source_channel = 0;
                     source_channel < channel_total;
                     ++source_channel) {
                    source_channels.push_back(SampleOutputChannelId{
                        .bundle = channel.source.bundle,
                        .port = channel.source.port,
                        .channel = source_channel,
                    });
                }
                plan.sample_producer_groups.push_back(SampleProducerGroupPlan{
                    .source_port = source_port,
                    .source_type = channel.source_layout.channel_type,
                    .source_channels = std::move(source_channels),
                    .canonical_source_layout = channel.source_layout,
                });
                group = std::prev(plan.sample_producer_groups.end());
            } else if (!group->canonical_source_layout
                || *group->canonical_source_layout != channel.source_layout) {
                // inventory_sample_connections() resolves one immutable output
                // declaration per source port, so disagreement here means the
                // semantic plan has already lost producer identity.
                continue;
            }
            if (std::ranges::find(group->connection_indices, i)
                == group->connection_indices.end()) {
                group->connection_indices.push_back(i);
            }
        }
    }

    for (std::size_t group_index = 0;
         group_index < plan.sample_producer_groups.size(); ++group_index) {
        auto& group = plan.sample_producer_groups[group_index];
        bool external = false;
        std::size_t retained = 0;
        for (auto const connection_index : group.connection_indices) {
            auto const& connection = plan.sample_connections[connection_index];
            if (!uses_realtime_storage(connection.access)) {
                group.has_indexed_connections = true;
                continue;
            }
            group.has_realtime_connections = true;
            // Feedback storage is branch-local: the canonical producer group
            // remains an ordinary aggregate sequence and detached branches
            // acquire their own persistent rings during physical lowering.
            external = external || connection.external_boundary;

            std::size_t connection_retained = 0;
            bool saw_group_channel = false;
            for (auto const& channel : connection.source_channel_timings) {
                if (!group.source_port
                    || source_port_for(channel.source) != *group.source_port) {
                    continue;
                }
                saw_group_channel = true;
                // A detached consumer owns its delay/history retention in
                // the branch-local retained feedback timeline. The canonical producer still
                // retains its own declared output history, which its callback
                // may address independently of any consumer.
                connection_retained = std::max(
                    connection_retained,
                    connection.detach
                        ? saturating_add(
                            channel.source_history, channel.source_latency)
                        : retained_extent(
                            channel.source_history,
                            channel.read_latency,
                            connection.target_history));
            }
            if (!saw_group_channel) continue;
            retained = std::max(retained, connection_retained);
        }
        group.storage_requirements = SampleConnectionStorageRequirements{
            .current_block_frames = kernel_block_size,
            .retained_frames = retained,
            .channel_count = group.canonical_source_layout
                ? channel_count(*group.canonical_source_layout)
                : channel_count(group.source_type),
            .value_size_bytes = sizeof(Sample),
        };
        if (!group.has_realtime_connections) continue;
        if (!external) {
            group.storage_plan = choose_sample_connection_storage_plan(
                group.storage_requirements, cost_model);
        }

        auto live = live_interval_for_sample_group(plan, group);
        live.crosses_kernel_invocations = live.crosses_kernel_invocations
            || group.storage_requirements.retained_frames != 0;
        group.live_interval = live;
        auto append_storage = [&](ConnectionStorageLifetime lifetime,
                                  std::size_t current_block_frames,
                                  std::size_t retained_extent_value) {
            plan.storage.regions.push_back(ConnectionStorageRegionRequirement{
                .payload = PlannedConnectionPayload::sample,
                .producer_group_index = group_index,
                .lifetime = lifetime,
                .live_interval = live,
                .current_block_frames = current_block_frames,
                .retained_extent = retained_extent_value,
                .channel_count = group.storage_requirements.channel_count,
                .value_size_bytes = sizeof(Sample),
            });
        };
        if (external) {
            append_storage(
                ConnectionStorageLifetime::external,
                kernel_block_size,
                group.storage_requirements.retained_frames);
            continue;
        }
        switch (group.storage_plan->kind) {
        case RealtimeBufferStorageKind::transient_stack:
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case RealtimeBufferStorageKind::stack_with_persistent_carry:
            append_storage(
                ConnectionStorageLifetime::persistent,
                0,
                group.storage_requirements.retained_frames);
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case RealtimeBufferStorageKind::full_node_storage:
            append_storage(
                ConnectionStorageLifetime::persistent,
                kernel_block_size,
                group.storage_requirements.retained_frames);
            break;
        }
    }
}

std::expected<void, std::string> plan_event_groups(
    ConnectionAnalysisPlan& plan,
    std::size_t kernel_block_size,
    RealtimeStorageCostModel const& cost_model)
{
    for (std::size_t i = 0; i < plan.event_connections.size(); ++i) {
        auto& connection = plan.event_connections[i];
        if (uses_realtime_storage(connection.access)) {
            std::optional<std::size_t> block_size;
            std::optional<std::size_t> common_region;
            bool same_cyclic_region = true;
            auto observe = [&](NodeBundleHandle bundle) {
                auto const current =
                    effective_block_size(plan, bundle, kernel_block_size);
                if (block_size && *block_size != current) {
                    connection.requires_block_materialization = true;
                } else {
                    block_size = current;
                }
                if (bundle >= plan.schedule.bundle_to_region.size()
                    || !plan.schedule.bundle_to_region[bundle]) {
                    same_cyclic_region = false;
                    return;
                }
                auto const region = *plan.schedule.bundle_to_region[bundle];
                if (region >= plan.schedule.regions.size()
                    || !plan.schedule.regions[region].cyclic) {
                    same_cyclic_region = false;
                    return;
                }
                if (common_region && *common_region != region) {
                    same_cyclic_region = false;
                } else {
                    common_region = region;
                }
            };
            for (auto const source : connection.sources) observe(source.bundle);
            for (auto const target : connection.targets) observe(target.bundle);
            // Slice-major SCC execution keeps exact-type event transport direct
            // inside one region. Outside an SCC, invocation-oriented sequences
            // still need root-block materialization whenever either endpoint is
            // sliced.
            if (!same_cyclic_region
                && block_size && *block_size != kernel_block_size) {
                connection.requires_block_materialization = true;
            }
        }
        auto group = std::ranges::find_if(
            plan.event_producer_groups,
            [&](EventProducerGroupPlan const& candidate) {
                return candidate.source_type == connection.source_type
                    && candidate.sources == connection.sources;
            });
        if (group == plan.event_producer_groups.end()) {
            plan.event_producer_groups.push_back(EventProducerGroupPlan{
                .source_type = connection.source_type,
                .sources = connection.sources,
                .max_events_per_index = connection.max_events_per_index,
            });
            group = std::prev(plan.event_producer_groups.end());
        }
        group->connection_indices.push_back(i);
    }

    for (std::size_t group_index = 0;
         group_index < plan.event_producer_groups.size(); ++group_index) {
        auto& group = plan.event_producer_groups[group_index];
        bool external = false;
        bool requires_invocation_aggregate = group.sources.size() > 1;
        std::size_t retained = 0;
        for (auto const source : group.sources) {
            if (source.bundle >= plan.schedule.bundle_to_region.size()
                || !plan.schedule.bundle_to_region[source.bundle]) {
                continue;
            }
            auto const region = *plan.schedule.bundle_to_region[source.bundle];
            requires_invocation_aggregate = requires_invocation_aggregate
                || (region < plan.schedule.regions.size()
                    && plan.schedule.regions[region].cyclic);
        }
        for (auto const connection_index : group.connection_indices) {
            auto const& connection = plan.event_connections[connection_index];
            if (!uses_realtime_storage(connection.access)) {
                group.has_indexed_connections = true;
                continue;
            }
            group.has_realtime_connections = true;
            // Feedback storage is branch-local: the canonical producer group
            // remains an ordinary aggregate sequence and detached branches
            // acquire their own persistent rings during physical lowering.
            external = external || connection.external_boundary;
            requires_invocation_aggregate = requires_invocation_aggregate
                || connection.requires_conversion
                || connection.requires_block_materialization
                || connection.detach.has_value();
            auto const connection_retained = retained_extent(
                connection.source_history,
                connection.source_latency,
                connection.target_history);
            retained = std::max(retained, connection_retained);
        }
        auto const current_event_capacity = event_count_for_sample_span(
            group.max_events_per_index, kernel_block_size);
        auto const retained_event_capacity = event_count_for_sample_span(
            group.max_events_per_index, retained);
        if (!current_event_capacity || !retained_event_capacity) {
            return std::unexpected(
                "GraphJit event producer sizing rate/sample span exceeds representable static capacity");
        }
        auto base_requirements = EventConnectionStorageRequirements{
            .current_window_samples = kernel_block_size,
            .retained_window_samples = retained,
            .current_event_capacity = *current_event_capacity,
            .retained_event_capacity = *retained_event_capacity,
            .value_size_bytes = sizeof(TimedEvent),
        };
        group.storage_requirements = base_requirements;
        group.requires_invocation_aggregate = requires_invocation_aggregate;
        if (!group.has_realtime_connections) continue;
        if (!external) {
            // Connection analysis owns only the event-rate and retained-window
            // requirements. Fan-in implementation alternatives depend on the
            // concrete per-slice producer buffers and merge operation selected
            // during lowering, so producer-home selection is deliberately
            // deferred to plan_event_ports().
            group.storage_plan = choose_event_connection_storage_plan(
                group.storage_requirements, cost_model);
        }

        auto live = live_interval_for_event_group(plan, group);
        live.crosses_kernel_invocations = live.crosses_kernel_invocations
            || retained != 0;
        group.live_interval = live;
        auto append_storage = [&](ConnectionStorageLifetime lifetime,
                                  std::size_t current_block_frames,
                                  std::size_t retained_extent_value) {
            plan.storage.regions.push_back(ConnectionStorageRegionRequirement{
                .payload = PlannedConnectionPayload::event,
                .producer_group_index = group_index,
                .lifetime = lifetime,
                .live_interval = live,
                .current_block_frames = current_block_frames,
                .retained_extent = retained_extent_value,
                .channel_count = 1,
                .value_size_bytes = 0,
            });
        };
        if (external) {
            append_storage(
                ConnectionStorageLifetime::external,
                kernel_block_size,
                retained);
            continue;
        }
        switch (group.storage_plan->kind) {
        case RealtimeBufferStorageKind::transient_stack:
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case RealtimeBufferStorageKind::stack_with_persistent_carry:
            append_storage(
                ConnectionStorageLifetime::persistent,
                0,
                retained);
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case RealtimeBufferStorageKind::full_node_storage:
            append_storage(
                ConnectionStorageLifetime::persistent,
                kernel_block_size,
                retained);
            break;
        }
    }
    return {};
}

} // namespace

std::expected<ConnectionAnalysisPlan, std::string> build_connection_analysis_plan(
    ConfiguredGraph const& graph,
    std::size_t kernel_block_size,
    RealtimeStorageCostModel const& cost_model,
    IndexedPlan const* retained_indexed_plan)
{
    if (kernel_block_size == 0) {
        return std::unexpected(
            "connection analysis requires a non-zero kernel block size");
    }

    ConnectionAnalysisPlan plan;
    if (auto inventory = inventory_nodes(graph, plan); !inventory) {
        return std::unexpected(std::move(inventory.error()));
    }
    if (auto samples = inventory_sample_connections(graph, plan); !samples) {
        return std::unexpected(std::move(samples.error()));
    }
    if (auto projections = normalize_sample_target_projections(graph, plan); !projections) {
        return std::unexpected(std::move(projections.error()));
    }
    if (auto events = inventory_event_connections(graph, plan); !events) {
        return std::unexpected(std::move(events.error()));
    }
    if (retained_indexed_plan) {
        if (retained_indexed_plan->semantic_nodes.size()
                != plan.nodes.size()
            || retained_indexed_plan->bundle_to_semantic_node.size()
                != graph.node_bundles.size()) {
            return std::unexpected(
                "GraphJit retained indexed plan does not match this graph specialization");
        }
        for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
            if (retained_indexed_plan->semantic_nodes[node].bundle
                != plan.nodes[node].bundle) {
                return std::unexpected(
                    "GraphJit retained indexed plan does not match configured node identity");
            }
        }
        plan.indexed = *retained_indexed_plan;
    } else {
        auto indexed = build_semantic_indexed_plan(graph, plan);
        if (!indexed) return std::unexpected(std::move(indexed.error()));
        if (auto populated = populate_indexed_topology(graph, plan, *indexed);
            !populated) {
            return std::unexpected(std::move(populated.error()));
        }
        plan.indexed = std::move(*indexed);
    }
    if (auto acyclic = validate_explicit_graph_is_acyclic(graph, plan); !acyclic) {
        return std::unexpected(std::move(acyclic.error()));
    }
    if (auto detaches = validate_detached_connections(graph, plan); !detaches) {
        return std::unexpected(std::move(detaches.error()));
    }
    auto schedule = build_schedule(graph, plan);
    if (!schedule) return std::unexpected(std::move(schedule.error()));
    plan.schedule = std::move(*schedule);
    if (auto detach_regions = bind_detach_regions(plan); !detach_regions) {
        return std::unexpected(std::move(detach_regions.error()));
    }
    if (auto latency = plan_sample_latency_compensation(plan); !latency) {
        return std::unexpected(std::move(latency.error()));
    }
    plan_sample_groups(plan, kernel_block_size, cost_model);
    if (auto events = plan_event_groups(
            plan, kernel_block_size, cost_model); !events) {
        return std::unexpected(std::move(events.error()));
    }
    return plan;
}

} // namespace iv::graph_jit::detail
