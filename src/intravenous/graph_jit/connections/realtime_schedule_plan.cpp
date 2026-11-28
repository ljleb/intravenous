#include <intravenous/graph_jit/connections/realtime_schedule_plan.h>

#include <intravenous/graph_jit/connection_plan.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>
#include <tuple>
#include <utility>

namespace iv::graph_jit::detail {
namespace {

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

std::size_t floor_power_of_two(std::size_t value) noexcept
{
    std::size_t result = 1;
    while (result <= value / 2) result *= 2;
    return result;
}

std::expected<SchedulePlan, std::string> build_schedule(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan,
    std::span<std::vector<std::size_t> const> explicit_outgoing,
    std::span<std::uint8_t const> callback_elided)
{
    SchedulePlan schedule;
    schedule.bundle_to_region.resize(graph.node_bundles.size());
    schedule.bundle_execution_position.resize(graph.node_bundles.size());
    schedule.callback_elided.assign(
        callback_elided.begin(), callback_elided.end());
    if (plan.nodes.empty()) return schedule;

    // Ordinary tick dependencies remain acyclic. Semantic SCC membership and
    // condensation order were already computed with every authored detach
    // restored while building BackgroundEvaluationPlan; schedule formation reuses those
    // facts and only derives the within-SCC same-slice order here.
    if (plan.background.semantic_nodes.size() != plan.nodes.size()
        || plan.background.semantic_sccs.empty()) {
        return std::unexpected(
            "GraphJit schedule is missing retained semantic SCC facts");
    }
    schedule.regions.reserve(plan.background.semantic_sccs.size());
    std::vector<std::optional<std::size_t>> node_to_region(plan.nodes.size());
    for (auto const& semantic_scc : plan.background.semantic_sccs) {
        auto const& component = semantic_scc.nodes;
        std::vector<std::size_t> tick_component;
        tick_component.reserve(component.size());
        for (auto const node : component) {
            auto const bundle = plan.nodes[node].bundle;
            if (plan.nodes[node].tick_execution
                && (bundle >= callback_elided.size()
                    || callback_elided[bundle] == 0)) {
                tick_component.push_back(node);
            }
        }
        if (tick_component.empty()) continue;

        auto const region_index = schedule.regions.size();
        SccRegionPlan region;
        region.maximum_block_size = std::numeric_limits<std::size_t>::max();
        for (auto const node : tick_component) {
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
        for (auto const node : tick_component) {
            for (auto const target : explicit_outgoing[node]) {
                if (std::ranges::find(tick_component, target)
                    != tick_component.end()) {
                    ++local_indegree[target];
                }
            }
        }
        std::vector<std::size_t> ready;
        for (auto const node : tick_component) {
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
                if (std::ranges::find(tick_component, target)
                    == tick_component.end()) continue;
                if (--local_indegree[target] == 0) {
                    auto const insertion = std::lower_bound(
                        ready.begin(), ready.end(), target, by_bundle);
                    ready.insert(insertion, target);
                }
            }
        }
        if (region.execution_order.size() != tick_component.size()) {
            return std::unexpected(
                "GraphJit SCC remains cyclic after detach dependencies are removed");
        }
        schedule.regions.push_back(std::move(region));
    }

    for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
        if (node_to_region[node]) {
            schedule.bundle_to_region[plan.nodes[node].bundle] =
                *node_to_region[node];
        }
    }

    return schedule;
}

std::expected<void, std::string> finalize_schedule(
    ConnectionAnalysisPlan const& plan,
    std::span<std::vector<std::size_t> const> explicit_outgoing,
    std::span<std::size_t const> callback_batch_classes,
    SchedulePlan& schedule)
{
    // Schedule condensation is a semantically distinct adjacency: background
    // dependencies must not pretend that tick_block produces their values.
    // Reuse the retained SCC partition, then order those SCCs using only
    // ordinary sequential Tick dependencies and retained delays between
    // regions. Detach binding has finalized each cyclic scheduling quantum.
    std::vector<std::optional<std::size_t>> node_to_region(plan.nodes.size());
    for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
        auto const bundle = plan.nodes[node].bundle;
        if (bundle < schedule.bundle_to_region.size()) {
            node_to_region[node] = schedule.bundle_to_region[bundle];
        }
    }
    std::vector<std::vector<std::size_t>> region_outgoing(
        schedule.regions.size());
    std::vector<std::size_t> region_indegree(schedule.regions.size(), 0);
    for (std::size_t source = 0; source < explicit_outgoing.size(); ++source) {
        for (auto const target : explicit_outgoing[source]) {
            if (!node_to_region[source] || !node_to_region[target]) {
                auto const source_bundle = plan.nodes[source].bundle;
                auto const target_bundle = plan.nodes[target].bundle;
                if ((source_bundle < schedule.callback_elided.size()
                        && schedule.callback_elided[source_bundle] != 0)
                    || (target_bundle < schedule.callback_elided.size()
                        && schedule.callback_elided[target_bundle] != 0)) {
                    continue;
                }
                return std::unexpected(
                    "GraphJit ordinary Tick dependency references a Tock-only primitive");
            }
            auto const source_region = *node_to_region[source];
            auto const target_region = *node_to_region[target];
            if (source_region == target_region) continue;
            if (schedule.regions[target_region].run_ahead && !schedule.regions[source_region].run_ahead) {
                return std::unexpected(
                    "GraphJit early Tick region depends on a foreground Tick region");
            }
            region_outgoing[source_region].push_back(target_region);
        }
    }
    for (auto& targets : region_outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (auto const target : targets) ++region_indegree[target];
    }
    auto region_key = [&](std::size_t region) {
        auto const& planned = schedule.regions[region];
        return std::pair{!planned.run_ahead, planned.nodes.front()};
    };
    auto callback_class = [&](NodeBundleHandle bundle)
        -> std::optional<std::size_t> {
        if (bundle >= callback_batch_classes.size()) return std::nullopt;
        auto const selected = callback_batch_classes[bundle];
        if (selected == std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        return selected;
    };
    auto batchable = [&](std::size_t region) {
        auto const& candidate = schedule.regions[region];
        if (callback_batch_classes.empty()
            || candidate.execution_order.empty()) {
            return false;
        }
        if (!candidate.cyclic && candidate.execution_order.size() != 1) {
            return false;
        }
        return std::ranges::all_of(
            candidate.execution_order,
            [&](NodeBundleHandle bundle) { return callback_class(bundle).has_value(); });
    };
    auto same_batch_class = [&](std::size_t left, std::size_t right) {
        auto const& a = schedule.regions[left];
        auto const& b = schedule.regions[right];
        if (!batchable(left) || !batchable(right) || a.run_ahead != b.run_ahead || a.cyclic != b.cyclic
            || a.execution_order.size() != b.execution_order.size()) {
            return false;
        }
        if (a.cyclic
            && (a.maximum_block_size != b.maximum_block_size
                || a.scc_feedback_latency != b.scc_feedback_latency)) {
            return false;
        }
        for (std::size_t phase = 0; phase < a.execution_order.size(); ++phase) {
            if (callback_class(a.execution_order[phase])
                != callback_class(b.execution_order[phase])) {
                return false;
            }
        }
        return true;
    };
    std::vector<std::size_t> ready_regions;
    for (std::size_t region = 0; region < schedule.regions.size(); ++region) {
        if (region_indegree[region] == 0) ready_regions.push_back(region);
    }
    std::ranges::sort(ready_regions, {}, region_key);
    while (!ready_regions.empty()) {
        auto selected_representative = ready_regions.front();
        bool const run_ahead = schedule.regions[selected_representative].run_ahead;
        std::size_t selected_count = 1;
        for (auto const candidate : ready_regions) {
            if (schedule.regions[candidate].run_ahead != run_ahead) break;
            if (!batchable(candidate)) continue;
            auto const count = static_cast<std::size_t>(std::ranges::count_if(
                ready_regions,
                [&](std::size_t region) {
                    return same_batch_class(region, candidate);
                }));
            if (count > selected_count) {
                selected_representative = candidate;
                selected_count = count;
            }
        }
        std::vector<std::size_t> selected;
        if (selected_count > 1) {
            for (auto const region : ready_regions) {
                if (same_batch_class(region, selected_representative)) {
                    selected.push_back(region);
                }
            }
        } else {
            selected.push_back(ready_regions.front());
        }
        for (std::size_t begin = 0; begin < selected.size();
             begin += initial_callback_batch_lane_limit) {
            auto const end = std::min(
                selected.size(), begin + initial_callback_batch_lane_limit);
            schedule.ready_groups.emplace_back(
                selected.begin() + static_cast<std::ptrdiff_t>(begin),
                selected.begin() + static_cast<std::ptrdiff_t>(end));
            if (run_ahead) ++schedule.early_ready_groups;
        }
        for (auto const region : selected) {
            schedule.region_order.push_back(region);
            auto const found = std::ranges::find(ready_regions, region);
            IV_ASSERT(found != ready_regions.end(),
                "selected ready SCC region is absent from the ready frontier");
            ready_regions.erase(found);
        }
        for (auto const region : selected) {
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
    }
    if (schedule.region_order.size() != schedule.regions.size()) {
        return std::unexpected(
            "GraphJit sequential SCC condensation graph is cyclic");
    }

    std::size_t execution_position = 0;
    for (auto const& group : schedule.ready_groups) {
        auto const& first = schedule.regions[group.front()];
        if (first.run_ahead) {
            for (auto const region : group)
                schedule.early_execution_steps += schedule.regions[region].execution_order.size();
        }
        auto const cyclic = schedule.regions[group.front()].cyclic;
        if (cyclic) {
            auto const phase_count = schedule.regions[group.front()]
                .execution_order.size();
            for (std::size_t phase = 0; phase < phase_count; ++phase) {
                for (auto const region_index : group) {
                    auto const bundle = schedule.regions[region_index]
                        .execution_order[phase];
                    schedule.bundle_execution_position[bundle] = execution_position++;
                }
            }
            continue;
        }
        for (auto const region_index : group) {
            for (auto const bundle : schedule.regions[region_index].execution_order) {
                schedule.bundle_execution_position[bundle] = execution_position++;
            }
        }
    }
    return {};
}

std::expected<void, std::string> bind_detach_regions(
    ConnectionAnalysisPlan& plan, std::span<std::vector<std::size_t>> tick_order)
{
    auto const& regions_by_bundle = plan.schedule.bundle_to_region;
    auto const& nodes_by_bundle = plan.background.bundle_to_semantic_node;
    for (auto& connection : plan.sample_connections) {
        if (!has_detached_source(connection)) continue;
        auto const target = connection.target_port.node_bundle_handle;
        if (target >= regions_by_bundle.size() || !regions_by_bundle[target]
            || !nodes_by_bundle[target]) {
            return std::unexpected("GraphJit sample detach has no concrete consumer");
        }
        auto const target_region = *regions_by_bundle[target];
        auto bind = [&](auto& detached, auto includes_source) {
            bool inside_loop = false;
            for (std::size_t i = 0; i < connection.source_channel_timings.size(); ++i) {
                if (!includes_source(i)) continue;
                auto const source = connection.source_channel_timings[i].source.bundle;
                if (source >= regions_by_bundle.size() || !regions_by_bundle[source]) continue;
                if (*regions_by_bundle[source] == target_region) {
                    inside_loop = true;
                    continue;
                }
                // A takeover can open the loop and leave this delay intact.
                // Produce the delayed stream before its consumer in that case.
                tick_order[*nodes_by_bundle[source]].push_back(*nodes_by_bundle[target]);
            }
            if (!inside_loop) return;
            detached.detach_region = target_region;
            auto& region = plan.schedule.regions[target_region];
            region.maximum_block_size = std::min(
                region.maximum_block_size, floor_power_of_two(detached.detach->loop_extra_latency));
        };
        if (connection.detach) bind(connection, [](std::size_t) { return true; });
        for (auto& contribution : connection.routed_contributions)
            if (contribution.detach) bind(contribution, [&](std::size_t i) {
                return contribution.source_coefficients[i] != 0.0f;
            });
    }
    for (auto& connection : plan.event_connections) {
        if (!has_detached_source(connection)) continue;
        if (connection.targets.empty())
            return std::unexpected("GraphJit detached event connection has no target");
        auto const target_bundle = connection.targets.front().bundle;
        std::optional<std::size_t> common_region;
        for (auto& source : connection.source_plans) {
            if (!source.detach) continue;
            if (target_bundle >= plan.schedule.bundle_to_region.size()
                || !plan.schedule.bundle_to_region[target_bundle]) {
                return std::unexpected(
                    "GraphJit event detach does not map to a concrete execution region");
            }
            auto const region_index =
                *plan.schedule.bundle_to_region[target_bundle];
            if (region_index >= plan.schedule.regions.size()
                || !plan.schedule.regions[region_index].cyclic) {
                return std::unexpected(
                    "GraphJit validated event detach did not form an execution SCC");
            }
            for (auto const target : connection.targets) {
                if (target.bundle >= plan.schedule.bundle_to_region.size()
                    || plan.schedule.bundle_to_region[target.bundle]
                        != region_index) {
                    return std::unexpected(
                        "GraphJit event detach targets span execution regions");
                }
            }
            source.detach_region = region_index;
            common_region = region_index;
            auto& region = plan.schedule.regions[region_index];
            region.maximum_block_size = std::min(
                region.maximum_block_size,
                floor_power_of_two(source.detach->loop_extra_latency));
        }
        if (connection.detach) connection.detach_region = common_region;
    }
    for (auto& region : plan.schedule.regions) {
        region.scc_feedback_latency = region.cyclic
            ? region.maximum_block_size
            : 0;
    }
    return {};
}

// Assign owners before ordering callbacks and allocating their storage.
// Signal flow includes Tock nodes and feedback edges.
void assign_tick_region_owners(ConnectionAnalysisPlan& plan)
{
    auto& schedule = plan.schedule;
    if (schedule.regions.empty()) return;
    auto const& topology = plan.background;
    std::vector<bool> live(topology.semantic_sccs.size());
    for (auto const index : topology.semantic_condensation_order) {
        auto const& scc = topology.semantic_sccs[index];
        live[index] = std::ranges::any_of(scc.nodes, [&](auto node) { return plan.nodes[node].is_live; })
            || std::ranges::any_of(scc.incoming, [&](auto predecessor) { return live[predecessor]; });
        for (auto const node : scc.nodes) {
            if (auto region = schedule.bundle_to_region[plan.nodes[node].bundle])
                schedule.regions[*region].run_ahead = !live[index];
        }
    }
}

void plan_early_tick_outputs(ConnectionAnalysisPlan& plan)
{
    auto& schedule = plan.schedule;
    auto runs_ahead = [&](NodeBundleHandle bundle) {
        auto const region = schedule.bundle_to_region[bundle];
        return region && schedule.regions[*region].run_ahead;
    };

    for (auto const& connection : plan.sample_connections) {
        if (runs_ahead(connection.target_port.node_bundle_handle)) continue;
        for (auto const& source : connection.source_channel_timings) {
            if (uses_realtime_storage(source.delivery) && runs_ahead(source.source.bundle))
                schedule.early_sample_outputs.push_back(source.source);
        }
    }
    for (auto const& connection : plan.event_connections) {
        for (auto const& delivery : connection.deliveries) {
            auto const source = connection.source_plans[delivery.source_index].source;
            auto const target = connection.target_plans[delivery.target_index].target;
            if (uses_realtime_storage(delivery.mechanism) && runs_ahead(source.bundle) && !runs_ahead(target.bundle))
                schedule.early_event_outputs.push_back(source);
        }
    }
    auto order_outputs = [](auto& outputs, auto key) {
        std::ranges::sort(outputs, {}, key);
        outputs.erase(std::ranges::unique(outputs).begin(), outputs.end());
    };
    order_outputs(schedule.early_sample_outputs, [](SampleOutputChannelId output) {
        return std::tuple{output.bundle, output.port, output.channel};
    });
    order_outputs(schedule.early_event_outputs, [](EventOutputPortId output) {
        return std::tuple{output.bundle, output.port};
    });
}

// One Tick-to-Sequential path into a concrete node's input, of either stream
// kind: the node it starts at, the authored latency of its source port, and the
// read latency that path-latency alignment assigns it. read_latency points into
// the connection plan that owns the path.
struct IncomingStreamPath {
    NodeBundleHandle source_bundle = 0;
    std::size_t source_latency = 0;
    std::size_t* read_latency = nullptr;
};

// Every feed-forward sample path into `bundle`: one per source channel of each
// entirely realtime, non-detached connection to one of its inputs. A detached
// (feedback) input is not aligned: its path latency includes the node's own
// output path latency plus its loop's, so aligning it would need a negative
// delay, and its read delay stays the authored one.
std::expected<void, std::string> append_sample_incoming_paths(
    ConnectionAnalysisPlan& plan,
    NodeBundleHandle bundle,
    std::vector<IncomingStreamPath>& paths)
{
    for (auto& route : plan.manual_sample_routes) {
        // Like the ordinary detached part of this expression, a detached
        // manual contribution keeps its authored delay rather than acquiring
        // feed-forward alignment across the temporal cut.
        if (route.delay == 0 && route.target.node_bundle_handle == bundle) paths.push_back({
            .source_bundle = plan.boundary_bundle, .source_latency = 0,
            .read_latency = &route.read_latency,
        });
    }
    for (auto& connection : plan.sample_connections) {
        if (!is_entirely_realtime_delivery(connection)
            || has_detached_source(connection)
            || connection.target_port.node_bundle_handle != bundle) {
            continue;
        }
        if (connection.source_channel_timings.size()
            != connection.source_channels.size()) {
            return std::unexpected(
                "latency compensation lost source-channel timing metadata");
        }
        for (auto& channel : connection.source_channel_timings) {
            paths.push_back(IncomingStreamPath{
                .source_bundle = channel.source.bundle,
                .source_latency = channel.source_latency,
                .read_latency = &channel.read_latency,
            });
        }
    }
    return {};
}

// Every feed-forward event path into `bundle`: one per Tick-to-Sequential
// delivery of each entirely realtime, non-detached connection to one of its
// inputs.
void append_event_incoming_paths(
    ConnectionAnalysisPlan& plan,
    NodeBundleHandle bundle,
    std::vector<IncomingStreamPath>& paths)
{
    for (auto& connection : plan.event_connections) {
        if (!is_entirely_realtime_delivery(connection)
            || has_detached_source(connection)) {
            continue;
        }
        for (auto& delivery : connection.deliveries) {
            if (delivery.target_index >= connection.target_plans.size()
                || delivery.source_index >= connection.source_plans.size()
                || connection.target_plans[delivery.target_index].target.bundle
                    != bundle) {
                continue;
            }
            auto const& source = connection.source_plans[delivery.source_index];
            paths.push_back(IncomingStreamPath{
                .source_bundle = source.source.bundle,
                .source_latency = source.latency,
                .read_latency = &delivery.read_latency,
            });
        }
    }
}

// A whole-port sample source has one producer path and one authored port
// latency, so every channel has the same read latency. A composed source keeps
// the largest of its channels' read latencies for whole-port planning;
// source_channel_timings stays authoritative.
void summarize_sample_read_latencies(
    ConnectionAnalysisPlan& plan,
    NodeBundleHandle bundle)
{
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
}

// Aligns every path into one node, whatever its stream kind: each path reads its
// source late enough that all of them arrive with the largest path latency into
// the node. Returns that latency.
std::expected<std::size_t, std::string> align_incoming_paths(
    ConnectionAnalysisPlan const& plan,
    std::span<IncomingStreamPath const> paths,
    std::span<std::optional<std::size_t> const> node_output_latency)
{
    std::vector<std::size_t> arrivals;
    arrivals.reserve(paths.size());
    std::size_t aligned = 0;
    for (auto const& path : paths) {
        std::size_t upstream = 0;
        if (path.source_bundle != plan.boundary_bundle) {
            if (path.source_bundle >= node_output_latency.size()
                || !node_output_latency[path.source_bundle]) {
                return std::unexpected(
                    "latency compensation encountered a source before its producer");
            }
            upstream = *node_output_latency[path.source_bundle];
        }
        auto arrival = checked_latency_add(
            upstream, path.source_latency, "stream path latency");
        if (!arrival) return std::unexpected(std::move(arrival.error()));
        aligned = std::max(aligned, *arrival);
        arrivals.push_back(*arrival);
    }
    for (std::size_t index = 0; index < paths.size(); ++index) {
        auto read_latency = checked_latency_add(
            paths[index].source_latency,
            aligned - arrivals[index],
            "compensated read latency");
        if (!read_latency) {
            return std::unexpected(std::move(read_latency.error()));
        }
        *paths[index].read_latency = *read_latency;
    }
    return aligned;
}

// Whole-graph feed-forward path-latency equalization. A concrete node's outputs
// carry the latency of its aligned inputs plus its internal latency, before the
// authored latency of any particular output port.
std::expected<void, std::string> plan_latency_compensation(
    ConfiguredGraph const& graph, ConnectionAnalysisPlan& plan)
{
    std::vector<std::optional<std::size_t>> node_output_latency(
        plan.schedule.bundle_to_region.size());

    auto planned_node = [&](NodeBundleHandle bundle) -> PlannedGraphNode const* {
        auto const found = std::ranges::find_if(
            plan.nodes,
            [&](PlannedGraphNode const& node) { return node.bundle == bundle; });
        return found == plan.nodes.end() ? nullptr : &*found;
    };

    // Keep authored output latency as the default for paths this Tick pass
    // does not align (background access, boundaries, and detached feedback
    // inputs, whose read delay is their authored one).
    for (auto& connection : plan.sample_connections) {
        // A private Tock gain view rescales retained values at their source
        // indices. Output latency controls live finalization, not an extra
        // time shift of the already indexed recording.
        auto const source_indices = reads_output_gain_source_indices(graph, connection.target_port);
        connection.read_latency = source_indices ? 0 : connection.source_latency;
        for (auto& channel : connection.source_channel_timings) {
            channel.read_latency = source_indices ? 0 : channel.source_latency;
        }
    }
    for (auto& connection : plan.event_connections) {
        for (auto& delivery : connection.deliveries) {
            delivery.read_latency =
                delivery.source_index < connection.source_plans.size()
                ? connection.source_plans[delivery.source_index].latency
                : 0;
        }
    }

    // A callback-elided producer has no inputs and belongs to no schedule
    // region. Its output path latency is its internal latency.
    for (std::size_t bundle = 0;
         bundle < plan.schedule.callback_elided.size(); ++bundle) {
        if (plan.schedule.callback_elided[bundle] == 0) continue;
        auto const* node = planned_node(bundle);
        if (!node || bundle >= node_output_latency.size()) {
            return std::unexpected(
                "latency compensation lost a callback-elided concrete node");
        }
        node_output_latency[bundle] = node->internal_latency_samples;
    }

    std::vector<IncomingStreamPath> paths;
    for (auto const region_index : plan.schedule.region_order) {
        if (region_index >= plan.schedule.regions.size()) {
            return std::unexpected(
                "latency compensation references an invalid schedule region");
        }
        auto const& region = plan.schedule.regions[region_index];

        // A cyclic region is aligned like an acyclic one. Its edges that are
        // not detached are acyclic, and execution_order is a topological order
        // of them, so every source a node's aligned paths start at has its
        // output path latency by then. A detached input keeps its authored
        // delay (append_*_incoming_paths()), so the loop's own latency is the
        // detach's, and compensation only adds delay on its forward edges.
        for (auto const bundle : region.execution_order) {
            auto const* node = planned_node(bundle);
            if (!node || bundle >= node_output_latency.size()) {
                return std::unexpected(
                    "latency compensation lost a scheduled concrete node");
            }
            paths.clear();
            if (auto appended = append_sample_incoming_paths(plan, bundle, paths);
                !appended) {
                return std::unexpected(std::move(appended.error()));
            }
            append_event_incoming_paths(plan, bundle, paths);
            auto aligned = align_incoming_paths(plan, paths, node_output_latency);
            if (!aligned) return std::unexpected(std::move(aligned.error()));
            summarize_sample_read_latencies(plan, bundle);

            auto output_latency = checked_latency_add(
                *aligned,
                node->internal_latency_samples,
                "node output path latency");
            if (!output_latency) {
                return std::unexpected(std::move(output_latency.error()));
            }
            node_output_latency[bundle] = *output_latency;
        }
    }

    return {};
}

} // namespace

std::expected<void, std::string> plan_realtime_schedule(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan,
    std::span<std::size_t const> callback_batch_classes,
    std::span<NodeBundleHandle const> callback_elision_candidates)
{
    std::vector<std::uint8_t> callback_elided(
        graph.node_bundles.size(), std::uint8_t{0});
    std::vector<std::uint8_t> callback_elision_seen(
        graph.node_bundles.size(), std::uint8_t{0});
    for (auto const bundle : callback_elision_candidates) {
        if (bundle >= callback_elided.size()) {
            return std::unexpected(
                "GraphJit callback-elision candidate references an invalid node bundle");
        }
        if (callback_elision_seen[bundle] != 0) {
            return std::unexpected(
                "GraphJit callback-elision candidate is listed more than once");
        }
        callback_elision_seen[bundle] = 1;
        bool has_realtime_consumer = false;
        bool has_background_consumer = false;
        for (auto const& connection : plan.sample_connections) {
            for (auto const& source : connection.source_channel_timings) {
                if (source.source.bundle != bundle) continue;
                if (uses_realtime_storage(source.delivery)) {
                    has_realtime_consumer = true;
                } else {
                    has_background_consumer = true;
                }
            }
        }
        callback_elided[bundle] = static_cast<std::uint8_t>(
            has_realtime_consumer && !has_background_consumer);
    }
    auto explicit_outgoing = dependency_adjacency(graph, plan, true);
    auto schedule = build_schedule(graph, plan, explicit_outgoing, callback_elided);
    if (!schedule) return std::unexpected(std::move(schedule.error()));
    plan.schedule = std::move(*schedule);
    if (auto detach_regions = bind_detach_regions(plan, explicit_outgoing); !detach_regions) {
        return std::unexpected(std::move(detach_regions.error()));
    }
    assign_tick_region_owners(plan);
    if (auto finalized = finalize_schedule(
            plan, explicit_outgoing, callback_batch_classes, plan.schedule);
        !finalized) {
        return std::unexpected(std::move(finalized.error()));
    }
    if (auto latency = plan_latency_compensation(graph, plan); !latency) {
        return std::unexpected(std::move(latency.error()));
    }
    plan_early_tick_outputs(plan);
    return {};
}

} // namespace iv::graph_jit::detail
