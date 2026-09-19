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
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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

PlannedConnectionAccess connection_access(
    bool source_realtime,
    bool target_realtime) noexcept
{
    if (source_realtime && target_realtime) {
        return PlannedConnectionAccess::realtime_to_realtime;
    }
    if (!source_realtime && !target_realtime) {
        return PlannedConnectionAccess::compiled_to_compiled;
    }
    return source_realtime
        ? PlannedConnectionAccess::realtime_to_compiled
        : PlannedConnectionAccess::compiled_to_realtime;
}

bool uses_realtime_storage(PlannedConnectionAccess access) noexcept
{
    return access == PlannedConnectionAccess::realtime_to_realtime;
}

bool has_sequential_source(PlannedConnectionAccess access) noexcept
{
    return access == PlannedConnectionAccess::realtime_to_realtime
        || access == PlannedConnectionAccess::realtime_to_compiled;
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
            connection_plan.target_history = realtime_history_or_zero(target);
            if (connection.detach) {
                if (connection.detach->loop_extra_latency == 0)
                    return std::unexpected(
                        "GraphJit sample detach latency must be at least one sample");
                connection_plan.detach = connection.detach;
                connection_plan.detach_initial_value =
                    connection.detach->initial_value_override.value_or(
                        target.neutral_value);
            }
            auto const target_realtime = is_realtime(target.access);

            std::optional<ChannelLayout> canonical_source_layout;
            std::optional<bool> source_realtime;
            connection_plan.source_channel_timings.reserve(
                connection.source_channels.size());
            for (auto const source_channel : connection.source_channels) {
                NodeBundlePortId const source_port{
                    source_channel.bundle, PortKind::sample, source_channel.port};
                auto const source =
                    graph.node_bundles.resolve_sample_output(source_port).config;
                auto const source_history = realtime_history_or_zero(source);
                auto const source_latency = realtime_latency_or_zero(source);
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
                auto const this_source_realtime = is_realtime(source.access);
                if (source_realtime && *source_realtime != this_source_realtime) {
                    return std::unexpected(
                        "sample connection sources mix realtime and compiled access");
                }
                source_realtime = this_source_realtime;
                if (connection_plan.canonical_source_port
                    && *connection_plan.canonical_source_port == source_port) {
                    canonical_source_layout = source.channel_layout;
                }
            }
            connection_plan.canonical_source_layout = canonical_source_layout;
            connection_plan.read_latency = connection_plan.source_latency;
            connection_plan.access = connection_access(
                source_realtime.value_or(true), target_realtime);
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
                    realtime_history_or_zero(source));
                connection_plan.source_latency = std::max(
                    connection_plan.source_latency,
                    realtime_latency_or_zero(source));
                if (!is_valid_event_buffer_rate(source.max_events_per_sample)) {
                    return std::unexpected(
                        "event output max_events_per_sample must be finite and nonnegative");
                }
                connection_plan.max_events_per_sample += source.max_events_per_sample;
                if (!is_valid_event_buffer_rate(connection_plan.max_events_per_sample)) {
                    return std::unexpected(
                        "event connection aggregate max_events_per_sample is not representable");
                }
                auto const this_source_realtime = is_realtime(source.access);
                if (source_realtime && *source_realtime != this_source_realtime) {
                    return std::unexpected(
                        "event connection sources mix realtime and compiled access");
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
                    realtime_history_or_zero(target));
                auto const this_target_realtime = is_realtime(target.access);
                if (target_realtime && *target_realtime != this_target_realtime) {
                    return std::unexpected(
                        "event connection targets mix realtime and compiled access");
                }
                target_realtime = this_target_realtime;
            }
            connection_plan.access = connection_access(
                source_realtime.value_or(true), target_realtime.value_or(true));
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

std::expected<void, std::string> validate_detached_connections(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan)
{
    auto const semantic_outgoing = dependency_adjacency(graph, plan, false);
    auto const sequential_outgoing = dependency_adjacency(graph, plan, true);
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
            bool closes_sequential_cycle = false;
            for (auto const source : sources) {
                if (source >= bundle_to_node.size() || !bundle_to_node[source])
                    continue;
                closes_semantic_cycle = closes_semantic_cycle
                    || has_path(
                        semantic_outgoing,
                        *bundle_to_node[target],
                        *bundle_to_node[source]);
                closes_sequential_cycle = closes_sequential_cycle
                    || has_path(
                        sequential_outgoing,
                        *bundle_to_node[target],
                        *bundle_to_node[source]);
            }
            if (!closes_semantic_cycle) {
                return std::unexpected(
                    "GraphJit " + std::string(kind) + " detach on connection "
                    + std::to_string(configured_connection_index)
                    + " breaks an acyclic dependency");
            }
            if (!closes_sequential_cycle) {
                return std::unexpected(
                    "GraphJit detach cycle crosses compiled-access dependencies, which SCC execution does not yet support");
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

    std::vector<std::optional<std::size_t>> bundle_to_node(
        graph.node_bundles.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        bundle_to_node[plan.nodes[i].bundle] = i;
    }

    // Ordinary tick dependencies remain acyclic. Restore detached semantic
    // source->consumer edges only for SCC formation. They are temporal/storage
    // dependencies, not same-slice tick dependencies.
    auto const explicit_outgoing = dependency_adjacency(graph, plan, true);
    auto augmented_outgoing = explicit_outgoing;
    std::vector<bool> self_loop(plan.nodes.size(), false);
    auto append_detach_dependency = [&](NodeBundleHandle source_bundle,
                                        NodeBundleHandle target_bundle)
        -> std::expected<void, std::string> {
        if (source_bundle >= bundle_to_node.size()
            || target_bundle >= bundle_to_node.size()
            || !bundle_to_node[source_bundle]
            || !bundle_to_node[target_bundle]) {
            return std::unexpected(
                "GraphJit detach scheduling refers to a non-concrete bundle");
        }
        auto const source = *bundle_to_node[source_bundle];
        auto const target = *bundle_to_node[target_bundle];
        if (source == target) self_loop[source] = true;
        augmented_outgoing[source].push_back(target);
        return {};
    };
    for (auto const& connection : plan.sample_connections) {
        if (!connection.detach) continue;
        for (auto const source : unique_source_bundles(connection.source_channels)) {
            if (auto added = append_detach_dependency(
                    source, connection.target_port.node_bundle_handle);
                !added) return std::unexpected(std::move(added.error()));
        }
    }
    for (auto const& connection : plan.event_connections) {
        if (!connection.detach) continue;
        for (auto const source : unique_source_bundles(connection.sources)) {
            for (auto const target : connection.targets) {
                if (auto added = append_detach_dependency(source, target.bundle);
                    !added) return std::unexpected(std::move(added.error()));
            }
        }
    }
    for (auto& targets : augmented_outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    }

    auto const unvisited = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> index(plan.nodes.size(), unvisited);
    std::vector<std::size_t> lowlink(plan.nodes.size(), 0);
    std::vector<bool> on_stack(plan.nodes.size(), false);
    std::vector<std::size_t> stack;
    std::size_t next_index = 0;
    std::vector<std::vector<std::size_t>> components;

    auto strongconnect = [&](auto&& self, std::size_t v) -> void {
        index[v] = next_index;
        lowlink[v] = next_index;
        ++next_index;
        stack.push_back(v);
        on_stack[v] = true;
        for (auto const w : augmented_outgoing[v]) {
            if (index[w] == unvisited) {
                self(self, w);
                lowlink[v] = std::min(lowlink[v], lowlink[w]);
            } else if (on_stack[w]) {
                lowlink[v] = std::min(lowlink[v], index[w]);
            }
        }
        if (lowlink[v] != index[v]) return;

        auto& component = components.emplace_back();
        while (true) {
            auto const w = stack.back();
            stack.pop_back();
            on_stack[w] = false;
            component.push_back(w);
            if (w == v) break;
        }
    };

    for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
        if (index[node] == unvisited) strongconnect(strongconnect, node);
    }

    schedule.regions.reserve(components.size());
    std::vector<std::size_t> node_to_region(plan.nodes.size(), 0);
    for (auto& component : components) {
        std::ranges::sort(component, [&](std::size_t a, std::size_t b) {
            return plan.nodes[a].bundle < plan.nodes[b].bundle;
        });
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
        region.cyclic = component.size() > 1
            || (component.size() == 1 && self_loop[component.front()]);

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

    std::vector<std::vector<std::size_t>> region_outgoing(schedule.regions.size());
    std::vector<std::size_t> indegree(schedule.regions.size(), 0);
    for (std::size_t source = 0; source < augmented_outgoing.size(); ++source) {
        for (auto const target : augmented_outgoing[source]) {
            auto const source_region = node_to_region[source];
            auto const target_region = node_to_region[target];
            if (source_region == target_region) continue;
            region_outgoing[source_region].push_back(target_region);
        }
    }
    for (auto& targets : region_outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (auto const target : targets) ++indegree[target];
    }

    auto region_key = [&](std::size_t region) {
        return schedule.regions[region].nodes.front();
    };
    std::vector<std::size_t> ready;
    for (std::size_t region = 0; region < schedule.regions.size(); ++region) {
        if (indegree[region] == 0) ready.push_back(region);
    }
    std::ranges::sort(ready, {}, region_key);
    while (!ready.empty()) {
        auto const region = ready.front();
        ready.erase(ready.begin());
        schedule.region_order.push_back(region);
        for (auto const target : region_outgoing[region]) {
            if (--indegree[target] == 0) {
                auto const insertion = std::lower_bound(
                    ready.begin(), ready.end(), target,
                    [&](std::size_t lhs, std::size_t rhs) {
                        return region_key(lhs) < region_key(rhs);
                    });
                ready.insert(insertion, target);
            }
        }
    }
    if (schedule.region_order.size() != schedule.regions.size()) {
        return std::unexpected("connection SCC condensation graph is cyclic");
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
    // by this feed-forward realtime pass (compiled access, boundaries, and
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
    std::size_t kernel_block_size)
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
        bool direct = true;
        bool requires_materialization = false;
        bool external = false;
        std::size_t retained = 0;
        for (auto const connection_index : group.connection_indices) {
            auto const& connection = plan.sample_connections[connection_index];
            if (!uses_realtime_storage(connection.access)) {
                group.has_compiled_connections = true;
                continue;
            }
            group.has_realtime_connections = true;
            // Feedback storage is branch-local: the canonical producer group
            // remains an ordinary aggregate sequence and detached branches
            // acquire their own persistent rings during physical lowering.
            external = external || connection.external_boundary;

            auto const canonical_branch = group.source_port
                && connection.canonical_source_port
                && *connection.canonical_source_port == *group.source_port;
            auto const branch_requires_materialization =
                !canonical_branch
                || connection.requires_conversion
                || connection.requires_block_materialization;
            requires_materialization = requires_materialization
                || branch_requires_materialization;

            std::size_t connection_retained = 0;
            bool saw_group_channel = false;
            for (auto const& channel : connection.source_channel_timings) {
                if (!group.source_port
                    || source_port_for(channel.source) != *group.source_port) {
                    continue;
                }
                saw_group_channel = true;
                // A detached consumer owns its delay/history retention in
                // the branch-local feedback ring. The canonical producer still
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
            direct = direct
                && canonical_branch
                && !connection.requires_conversion
                && !connection.requires_block_materialization
                && !connection.detach
                && !connection.external_boundary
                && connection_retained == 0;
        }
        group.requirements = SampleConnectionImplementationRequirements{
            .direct_implementation_legal = direct,
            .requires_materialization = requires_materialization,
            .feedback = false,
            .external_boundary = external,
            .retained_frames = retained,
            .channel_count = group.canonical_source_layout
                ? channel_count(*group.canonical_source_layout)
                : channel_count(group.source_type),
            .value_size_bytes = sizeof(Sample),
        };
        if (!group.has_realtime_connections) continue;
        group.implementation = choose_sample_connection_implementation(
            group.requirements);

        auto live = live_interval_for_sample_group(plan, group);
        live.crosses_kernel_invocations = live.crosses_kernel_invocations
            || group.requirements.retained_frames != 0;
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
                .channel_count = group.requirements.channel_count,
                .value_size_bytes = group.requirements.value_size_bytes,
            });
        };
        switch (*group.implementation) {
        case SampleConnectionImplementationKind::direct:
            break;
        case SampleConnectionImplementationKind::transient_materialization:
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case SampleConnectionImplementationKind::compact_persistent_carry:
            append_storage(
                ConnectionStorageLifetime::persistent,
                0,
                group.requirements.retained_frames);
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case SampleConnectionImplementationKind::persistent_ring:
        case SampleConnectionImplementationKind::feedback_ring:
            append_storage(
                ConnectionStorageLifetime::persistent,
                kernel_block_size,
                group.requirements.retained_frames);
            break;
        case SampleConnectionImplementationKind::external_boundary:
            append_storage(
                ConnectionStorageLifetime::external,
                kernel_block_size,
                group.requirements.retained_frames);
            break;
        }
    }
}

void plan_event_groups(
    ConnectionAnalysisPlan& plan,
    std::size_t kernel_block_size)
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
                .max_events_per_sample = connection.max_events_per_sample,
            });
            group = std::prev(plan.event_producer_groups.end());
        }
        group->connection_indices.push_back(i);
    }

    for (std::size_t group_index = 0;
         group_index < plan.event_producer_groups.size(); ++group_index) {
        auto& group = plan.event_producer_groups[group_index];
        bool direct = true;
        bool requires_materialization = false;
        bool external = false;
        std::size_t retained = 0;
        bool source_in_cyclic_region = false;
        for (auto const source : group.sources) {
            if (source.bundle >= plan.schedule.bundle_to_region.size()
                || !plan.schedule.bundle_to_region[source.bundle]) {
                continue;
            }
            auto const region = *plan.schedule.bundle_to_region[source.bundle];
            source_in_cyclic_region = source_in_cyclic_region
                || (region < plan.schedule.regions.size()
                    && plan.schedule.regions[region].cyclic);
        }
        for (auto const connection_index : group.connection_indices) {
            auto const& connection = plan.event_connections[connection_index];
            if (!uses_realtime_storage(connection.access)) {
                group.has_compiled_connections = true;
                continue;
            }
            group.has_realtime_connections = true;
            // Feedback storage is branch-local: the canonical producer group
            // remains an ordinary aggregate sequence and detached branches
            // acquire their own persistent rings during physical lowering.
            external = external || connection.external_boundary;
            requires_materialization = requires_materialization
                || connection.requires_conversion
                || connection.requires_block_materialization;
            auto const connection_retained = retained_extent(
                connection.source_history,
                connection.source_latency,
                connection.target_history);
            retained = std::max(retained, connection_retained);
            direct = direct
                && !connection.requires_conversion
                && !connection.requires_block_materialization
                && !connection.detach
                && !connection.external_boundary
                && connection_retained == 0;
        }
        std::optional<std::size_t> retained_event_capacity;
        if (retained != 0) {
            retained_event_capacity = event_count_for_sample_span(
                group.max_events_per_sample, retained);
        }
        // A cyclic region executes slice-major. Keep one aggregate producer
        // sequence for the whole root call so per-slice callbacks do not reset
        // and thereby enlarge the authored static event budget. Same-region
        // consumers still bind that representation directly and select their
        // current absolute-time window.
        requires_materialization = requires_materialization
            || source_in_cyclic_region;
        direct = direct && !source_in_cyclic_region;
        group.requirements = EventConnectionImplementationRequirements{
            .direct_implementation_legal = direct,
            .requires_materialization = requires_materialization,
            .feedback = false,
            .external_boundary = external,
            .retained_window_samples = retained,
            .retained_event_capacity = retained_event_capacity,
        };
        if (!group.has_realtime_connections) continue;
        group.implementation = choose_event_connection_implementation(
            group.requirements);

        auto live = live_interval_for_event_group(plan, group);
        live.crosses_kernel_invocations = live.crosses_kernel_invocations
            || group.requirements.retained_window_samples != 0;
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
        switch (*group.implementation) {
        case EventConnectionImplementationKind::direct:
            break;
        case EventConnectionImplementationKind::transient_sequence:
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case EventConnectionImplementationKind::compact_persistent_carry:
            append_storage(
                ConnectionStorageLifetime::persistent,
                0,
                group.requirements.retained_window_samples);
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case EventConnectionImplementationKind::persistent_ring:
        case EventConnectionImplementationKind::feedback_ring:
            append_storage(
                ConnectionStorageLifetime::persistent,
                kernel_block_size,
                group.requirements.retained_window_samples);
            break;
        case EventConnectionImplementationKind::external_boundary:
            append_storage(
                ConnectionStorageLifetime::external,
                kernel_block_size,
                group.requirements.retained_window_samples);
            break;
        }
    }
}

} // namespace

std::expected<ConnectionAnalysisPlan, std::string> build_connection_analysis_plan(
    ConfiguredGraph const& graph,
    std::size_t kernel_block_size)
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
    plan_sample_groups(plan, kernel_block_size);
    plan_event_groups(plan, kernel_block_size);
    return plan;
}

} // namespace iv::graph_jit::detail
