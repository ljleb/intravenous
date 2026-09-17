#include <intravenous/graph_jit/connection_plan.h>

#include <intravenous/channel_layout.h>
#include <intravenous/ports.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
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
    std::size_t source_latency,
    std::size_t target_history) noexcept
{
    return saturating_add(
        std::max(source_history, target_history), source_latency);
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
            connection_plan.target_history = realtime_history_or_zero(target);
            auto const target_realtime = is_realtime(target.access);

            std::optional<ChannelLayout> canonical_source_layout;
            std::optional<bool> source_realtime;
            for (auto const source_channel : connection.source_channels) {
                NodeBundlePortId const source_port{
                    source_channel.bundle, PortKind::sample, source_channel.port};
                auto const source =
                    graph.node_bundles.resolve_sample_output(source_port).config;
                connection_plan.source_history = std::max(
                    connection_plan.source_history,
                    realtime_history_or_zero(source));
                connection_plan.source_latency = std::max(
                    connection_plan.source_latency,
                    realtime_latency_or_zero(source));
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
            connection_plan.access = connection_access(
                source_realtime.value_or(true), target_realtime);
            for (auto const source_channel : connection.source_channels) {
                append_dependency(
                    plan,
                    source_channel.bundle,
                    first_target.bundle,
                    PlannedConnectionPayload::sample,
                    connection_plan.access,
                    i);
            }
            connection_plan.external_boundary =
                first_target.bundle == plan.boundary_bundle
                || std::ranges::any_of(
                    connection.source_channels,
                    [&](SampleOutputChannelId source) {
                        return source.bundle == plan.boundary_bundle;
                    });
            connection_plan.requires_conversion =
                !connection_plan.canonical_source_port
                || connection.source_type != connection.target_type
                || !canonical_source_layout
                || *canonical_source_layout != target.channel_layout;
            if (connection_plan.requires_conversion) {
                // Validate now that a semantic sample-layout conversion exists;
                // point 6 will decide how to realize the returned plan.
                auto const source_layout = canonical_source_layout.value_or(
                    ChannelLayout{
                        .channel_type = connection.source_type,
                        .sample_layout = SampleStreamLayout::planar,
                    });
                (void)ChannelConversionRegistry::plan(
                    source_layout, target.channel_layout);
            }
        } catch (std::exception const& e) {
            return std::unexpected(
                "sample connection analysis failed: " + std::string(e.what()));
        }
        plan.sample_connections.push_back(std::move(connection_plan));
    }
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
            connection_plan.requires_conversion =
                connection.source_type != connection.target_type
                || connection.sources.size() != 1;
            (void)EventConversionRegistry::instance().plan(
                connection.source_type, connection.target_type);
        } catch (std::exception const& e) {
            return std::unexpected(
                "event connection analysis failed: " + std::string(e.what()));
        }
        plan.event_connections.push_back(std::move(connection_plan));
    }
    return {};
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

    std::vector<std::vector<std::size_t>> outgoing(plan.nodes.size());
    std::vector<bool> self_loop(plan.nodes.size(), false);
    for (auto const& dependency : plan.dependencies) {
        if (!dependency.sequential_tick_dependency) continue;
        if (dependency.source_bundle >= bundle_to_node.size()
            || dependency.target_bundle >= bundle_to_node.size()
            || !bundle_to_node[dependency.source_bundle]
            || !bundle_to_node[dependency.target_bundle]) {
            return std::unexpected(
                "connection dependency refers to a non-concrete bundle");
        }
        auto const source = *bundle_to_node[dependency.source_bundle];
        auto const target = *bundle_to_node[dependency.target_bundle];
        if (source == target) self_loop[source] = true;
        outgoing[source].push_back(target);
    }
    for (auto& targets : outgoing) {
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
        for (auto const w : outgoing[v]) {
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
        SccRegionPlan region;
        region.maximum_block_size = std::numeric_limits<std::size_t>::max();
        for (auto const node : component) {
            node_to_region[node] = schedule.regions.size();
            region.nodes.push_back(plan.nodes[node].bundle);
            region.execution_order.push_back(plan.nodes[node].bundle);
            region.maximum_block_size = std::min(
                region.maximum_block_size,
                plan.nodes[node].maximum_block_size);
        }
        region.cyclic = component.size() > 1
            || (component.size() == 1 && self_loop[component.front()]);
        schedule.regions.push_back(std::move(region));
    }

    for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
        schedule.bundle_to_region[plan.nodes[node].bundle] = node_to_region[node];
    }

    std::vector<std::vector<std::size_t>> region_outgoing(schedule.regions.size());
    std::vector<std::size_t> indegree(schedule.regions.size(), 0);
    for (std::size_t source = 0; source < outgoing.size(); ++source) {
        for (auto const target : outgoing[source]) {
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
        for (auto const bundle : schedule.regions[region_index].execution_order) {
            schedule.bundle_execution_position[bundle] = execution_position++;
        }
    }
    return schedule;
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

bool is_feedback(
    SchedulePlan const& schedule,
    NodeBundleHandle boundary,
    NodeBundleHandle source,
    NodeBundleHandle target)
{
    if (source == boundary || target == boundary
        || source >= schedule.bundle_to_region.size()
        || target >= schedule.bundle_to_region.size()
        || !schedule.bundle_to_region[source]
        || !schedule.bundle_to_region[target]) {
        return false;
    }
    auto const source_region = *schedule.bundle_to_region[source];
    auto const target_region = *schedule.bundle_to_region[target];
    return source_region == target_region
        && schedule.regions[source_region].cyclic;
}

void mark_feedback(ConnectionAnalysisPlan& plan)
{
    for (auto& connection : plan.sample_connections) {
        connection.feedback = std::ranges::any_of(
            connection.source_channels,
            [&](SampleOutputChannelId source) {
                return is_feedback(
                    plan.schedule,
                    plan.boundary_bundle,
                    source.bundle,
                    connection.target_port.node_bundle_handle);
            });
    }
    for (auto& connection : plan.event_connections) {
        connection.feedback = std::ranges::any_of(
            connection.sources,
            [&](EventOutputPortId source) {
                return std::ranges::any_of(
                    connection.targets,
                    [&](EventInputPortId target) {
                        return is_feedback(
                            plan.schedule,
                            plan.boundary_bundle,
                            source.bundle,
                            target.bundle);
                    });
            });
    }
}

ConnectionLiveIntervalPlan live_interval_for_sample_group(
    ConnectionAnalysisPlan const& plan,
    SampleProducerGroupPlan const& group)
{
    auto const execution_count = plan.nodes.size();
    ConnectionLiveIntervalPlan live{
        .begin = execution_count,
        .end = 0,
    };
    bool saw_endpoint = false;
    for (auto const connection_index : group.connection_indices) {
        auto const& connection = plan.sample_connections[connection_index];
        if (!uses_realtime_storage(connection.access)) continue;
        for (auto const source : connection.source_channels) {
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
            live.crosses_kernel_invocations || connection.feedback;
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
    auto const execution_count = plan.nodes.size();
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
            live.crosses_kernel_invocations || connection.feedback;
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
    for (std::size_t i = 0; i < plan.sample_connections.size(); ++i) {
        auto& connection = plan.sample_connections[i];
        if (uses_realtime_storage(connection.access)
            && connection.canonical_source_port) {
            connection.requires_block_materialization =
                effective_block_size(
                    plan,
                    connection.canonical_source_port->node_bundle_handle,
                    kernel_block_size)
                != effective_block_size(
                    plan,
                    connection.target_port.node_bundle_handle,
                    kernel_block_size);
        }
        auto group = std::ranges::find_if(
            plan.sample_producer_groups,
            [&](SampleProducerGroupPlan const& candidate) {
                return candidate.source_type == connection.source_type
                    && candidate.source_channels == connection.source_channels;
            });
        if (group == plan.sample_producer_groups.end()) {
            plan.sample_producer_groups.push_back(SampleProducerGroupPlan{
                .source_type = connection.source_type,
                .source_channels = connection.source_channels,
            });
            group = std::prev(plan.sample_producer_groups.end());
        }
        group->connection_indices.push_back(i);
    }

    for (std::size_t group_index = 0;
         group_index < plan.sample_producer_groups.size(); ++group_index) {
        auto& group = plan.sample_producer_groups[group_index];
        bool direct = true;
        bool requires_materialization = false;
        bool feedback = false;
        bool external = false;
        std::size_t retained = 0;
        for (auto const connection_index : group.connection_indices) {
            auto const& connection = plan.sample_connections[connection_index];
            if (!uses_realtime_storage(connection.access)) {
                group.has_compiled_connections = true;
                continue;
            }
            group.has_realtime_connections = true;
            feedback = feedback || connection.feedback;
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
                && !connection.feedback
                && !connection.external_boundary
                && connection_retained == 0;
        }
        group.requirements = SampleConnectionImplementationRequirements{
            .direct_implementation_legal = direct,
            .requires_materialization = requires_materialization,
            .feedback = feedback,
            .external_boundary = external,
            .retained_frames = retained,
            .channel_count = channel_count(group.source_type),
            .value_size_bytes = sizeof(Sample),
        };
        if (!group.has_realtime_connections) continue;
        group.implementation = choose_sample_connection_implementation(
            group.requirements);

        auto live = live_interval_for_sample_group(plan, group);
        live.crosses_kernel_invocations = live.crosses_kernel_invocations
            || group.requirements.retained_frames != 0;
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
            auto observe = [&](NodeBundleHandle bundle) {
                auto const current =
                    effective_block_size(plan, bundle, kernel_block_size);
                if (block_size && *block_size != current) {
                    connection.requires_block_materialization = true;
                } else {
                    block_size = current;
                }
            };
            for (auto const source : connection.sources) observe(source.bundle);
            for (auto const target : connection.targets) observe(target.bundle);
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
        bool feedback = false;
        bool external = false;
        std::size_t retained = 0;
        for (auto const connection_index : group.connection_indices) {
            auto const& connection = plan.event_connections[connection_index];
            if (!uses_realtime_storage(connection.access)) {
                group.has_compiled_connections = true;
                continue;
            }
            group.has_realtime_connections = true;
            feedback = feedback || connection.feedback;
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
                && !connection.feedback
                && !connection.external_boundary
                && connection_retained == 0;
        }
        group.requirements = EventConnectionImplementationRequirements{
            .direct_implementation_legal = direct,
            .requires_materialization = requires_materialization,
            .feedback = feedback,
            .external_boundary = external,
            .retained_window_samples = retained,
            .estimated_retained_events = std::nullopt,
        };
        if (!group.has_realtime_connections) continue;
        group.implementation = choose_event_connection_implementation(
            group.requirements);

        auto live = live_interval_for_event_group(plan, group);
        live.crosses_kernel_invocations = live.crosses_kernel_invocations
            || group.requirements.retained_window_samples != 0;
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
    if (!graph.virtual_nodes.records().empty()) {
        return std::unexpected(
            "connection analysis does not yet support virtual nodes");
    }
    if (auto samples = inventory_sample_connections(graph, plan); !samples) {
        return std::unexpected(std::move(samples.error()));
    }
    if (auto events = inventory_event_connections(graph, plan); !events) {
        return std::unexpected(std::move(events.error()));
    }
    auto schedule = build_schedule(graph, plan);
    if (!schedule) return std::unexpected(std::move(schedule.error()));
    plan.schedule = std::move(*schedule);
    mark_feedback(plan);
    plan_sample_groups(plan, kernel_block_size);
    plan_event_groups(plan, kernel_block_size);
    return plan;
}

} // namespace iv::graph_jit::detail
