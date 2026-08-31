#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/executable_graph_ir.hpp>
#include <intravenous/graph/error.h>
#include <intravenous/graph/names.h>
#include <intravenous/graph/node.h>
#include <intravenous/graph/reflected_node.hpp>
#include <intravenous/graph/types.h>
#include <intravenous/graph/wiring.h>

#include <algorithm>
#include <bit>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <flat_map>
#include <flat_set>
#include <utility>
#include <vector>

namespace iv::details {
    constexpr size_t floor_power_of_2(size_t value)
    {
        if (value == 0) {
            return 0;
        }
        return std::bit_floor(value);
    }


    constexpr std::vector<DormancyGroup> compile_dormancy_groups(
        ExecutableGraphData const& g,
        std::span<LoweredSubgraph const> lowered_subgraphs,
        std::string_view graph_id,
        GraphExecutionPlan const& execution_plan
    )
    {
        std::vector<size_t> region_to_order(execution_plan.regions.size(), GRAPH_ID);
        for (size_t ordered_i = 0; ordered_i < execution_plan.region_order.size(); ++ordered_i) {
            region_to_order[execution_plan.region_order[ordered_i]] = ordered_i;
        }

        std::vector<DormancyGroup> groups;
        groups.reserve(lowered_subgraphs.size());
        std::vector<std::flat_set<size_t>> member_sets;
        member_sets.reserve(lowered_subgraphs.size());

        for (auto const& lowered_subgraph : lowered_subgraphs) {
            DormancyGroup group;
            group.parent_group = lowered_subgraph.parent_scope;
            group.ttl_samples = lowered_subgraph.ttl_samples;
            group.member_nodes = lowered_subgraph.member_nodes;

            std::flat_set<size_t> member_set(
                group.member_nodes.begin(),
                group.member_nodes.end()
            );
            member_sets.push_back(std::move(member_set));
            groups.push_back(std::move(group));
        }

        for (size_t group_i = 0; group_i < groups.size(); ++group_i) {
            auto const& member_set = member_sets[group_i];
            std::flat_set<std::string> seen_sample_inputs;
            std::flat_set<std::string> seen_event_inputs;
            std::flat_set<std::string> seen_sample_outputs;

            for (GraphEdge const& edge : g.edges) {
                bool const target_inside = edge.target.node != GRAPH_ID && member_set.contains(edge.target.node);
                bool const source_inside = edge.source.node != GRAPH_ID && member_set.contains(edge.source.node);

                if (target_inside && !source_inside) {
                    auto const export_id = port_data_export_id(g.node_ids[edge.target.node], edge.target.port);
                    if (seen_sample_inputs.insert(export_id).second) {
                        groups[group_i].sample_input_frontier.push_back(DormancySamplePort{
                            .export_id = export_id,
                            .history = g.nodes[edge.target.node].inputs()[edge.target.port].history,
                        });
                    }
                    size_t const ordered_region = region_to_order[execution_plan.node_to_region[edge.target.node]];
                    if (ordered_region != GRAPH_ID) {
                        groups[group_i].wake_check_regions.push_back(ordered_region);
                    }
                }

                if (source_inside && !target_inside) {
                    std::string export_id;
                    size_t history = 0;
                    if (edge.target.node == GRAPH_ID) {
                        export_id = graph_port_data_export_id(graph_id, edge.target.port);
                    } else {
                        export_id = port_data_export_id(g.node_ids[edge.target.node], edge.target.port);
                        history = g.nodes[edge.target.node].inputs()[edge.target.port].history;
                    }
                    if (seen_sample_outputs.insert(export_id).second) {
                        groups[group_i].sample_output_frontier.push_back(DormancySamplePort{
                            .export_id = std::move(export_id),
                            .history = history,
                        });
                    }
                }
            }

            for (GraphEventEdge const& edge : g.event_edges) {
                bool const target_inside = edge.target.node != GRAPH_ID && member_set.contains(edge.target.node);
                bool const source_inside = edge.source.node != GRAPH_ID && member_set.contains(edge.source.node);
                if (target_inside && !source_inside) {
                    auto const export_id = event_port_data_export_id(g.node_ids[edge.target.node], edge.target.port);
                    if (seen_event_inputs.insert(export_id).second) {
                        groups[group_i].event_input_frontier.push_back(DormancyEventPort{
                            .export_id = export_id,
                        });
                    }
                    size_t const ordered_region = region_to_order[execution_plan.node_to_region[edge.target.node]];
                    if (ordered_region != GRAPH_ID) {
                        groups[group_i].wake_check_regions.push_back(ordered_region);
                    }
                }
            }

            std::sort(groups[group_i].wake_check_regions.begin(), groups[group_i].wake_check_regions.end());
            groups[group_i].wake_check_regions.erase(
                std::unique(groups[group_i].wake_check_regions.begin(), groups[group_i].wake_check_regions.end()),
                groups[group_i].wake_check_regions.end()
            );

            groups[group_i].can_skip =
                !groups[group_i].member_nodes.empty()
                && !groups[group_i].sample_output_frontier.empty()
                && (!groups[group_i].sample_input_frontier.empty() || !groups[group_i].event_input_frontier.empty())
                && std::all_of(
                    groups[group_i].member_nodes.begin(),
                    groups[group_i].member_nodes.end(),
                    [&](size_t runtime_node) { return g.nodes[runtime_node].can_skip_block(); }
                );
        }

        std::vector<size_t> kept_old_indices;
        kept_old_indices.reserve(groups.size());
        for (size_t i = 0; i < groups.size(); ++i) {
            if (groups[i].can_skip) {
                kept_old_indices.push_back(i);
            }
        }

        std::flat_map<size_t, size_t> remapped_group_indices;
        for (size_t new_i = 0; new_i < kept_old_indices.size(); ++new_i) {
            remapped_group_indices.emplace(kept_old_indices[new_i], new_i);
        }

        std::vector<DormancyGroup> filtered;
        filtered.reserve(kept_old_indices.size());
        for (size_t old_i : kept_old_indices) {
            DormancyGroup group = std::move(groups[old_i]);
            size_t parent = group.parent_group;
            while (parent != GRAPH_ID && !remapped_group_indices.contains(parent)) {
                parent = groups[parent].parent_group;
            }
            group.parent_group = parent == GRAPH_ID ? GRAPH_ID : remapped_group_indices.at(parent);
            filtered.push_back(std::move(group));
        }

        return filtered;
    }

    constexpr std::vector<LoweredSubgraph> compile_lowered_subgraphs(
        ExecutableGraphData const& g,
        std::span<LoweredSubgraphSpec const> scopes
    )
    {
        std::flat_map<std::string, size_t> runtime_index_by_node_id;
        for (size_t i = 0; i < g.node_ids.size(); ++i) {
            runtime_index_by_node_id.emplace(g.node_ids[i], i);
        }

        std::vector<LoweredSubgraph> lowered_subgraphs;
        lowered_subgraphs.reserve(scopes.size());
        for (auto const& scope : scopes) {
            LoweredSubgraph lowered;
            lowered.parent_scope = scope.parent_scope;
            lowered.kind = scope.kind;
            lowered.backing_node_id = scope.backing_node_id;
            lowered.source_spans = scope.source_spans;
            lowered.sample_inputs = scope.sample_inputs;
            lowered.sample_outputs = scope.sample_outputs;
            lowered.event_inputs = scope.event_inputs;
            lowered.event_outputs = scope.event_outputs;
            lowered.ttl_samples = scope.ttl_samples;

            auto remap_port = [&](LoweredSubgraphSpec::PortRef const& port) {
                if (port.is_graph_port) {
                    return ConcretePortId{ GRAPH_ID, port.port };
                }
                auto const it = runtime_index_by_node_id.find(port.node_id);
                if (it == runtime_index_by_node_id.end()) {
                    return ConcretePortId{ GRAPH_ID, port.port };
                }
                return ConcretePortId{ it->second, port.port };
            };

            for (auto const& node_id : scope.member_node_ids) {
                auto const it = runtime_index_by_node_id.find(node_id);
                if (it == runtime_index_by_node_id.end()) {
                    continue;
                }
                lowered.member_nodes.push_back(it->second);
            }

            lowered.sample_input_targets.reserve(scope.sample_input_targets.size());
            for (auto const& targets : scope.sample_input_targets) {
                std::vector<ConcretePortId> remapped_targets;
                remapped_targets.reserve(targets.size());
                for (auto const& target : targets) {
                    remapped_targets.push_back(remap_port(target));
                }
                lowered.sample_input_targets.push_back(std::move(remapped_targets));
            }
            lowered.sample_output_sources.reserve(scope.sample_output_sources.size());
            for (auto const& source : scope.sample_output_sources) {
                lowered.sample_output_sources.push_back(remap_port(source));
            }
            lowered.event_input_targets.reserve(scope.event_input_targets.size());
            for (auto const& targets : scope.event_input_targets) {
                std::vector<ConcretePortId> remapped_targets;
                remapped_targets.reserve(targets.size());
                for (auto const& target : targets) {
                    remapped_targets.push_back(remap_port(target));
                }
                lowered.event_input_targets.push_back(std::move(remapped_targets));
            }
            lowered.event_output_sources.reserve(scope.event_output_sources.size());
            for (auto const& source : scope.event_output_sources) {
                lowered.event_output_sources.push_back(remap_port(source));
            }

            std::sort(lowered.member_nodes.begin(), lowered.member_nodes.end());
            lowered.member_nodes.erase(
                std::unique(lowered.member_nodes.begin(), lowered.member_nodes.end()),
                lowered.member_nodes.end()
            );
            if (lowered.member_nodes.empty()) {
                continue;
            }

            lowered_subgraphs.push_back(std::move(lowered));
        }

        return lowered_subgraphs;
    }
    constexpr auto make_node_adjacency(ExecutableGraphData const& g)
    {
        size_t const num_nodes = g.nodes.size();
        std::vector<std::flat_set<size_t>> outgoing(num_nodes);
        std::vector<size_t> indegree(num_nodes, 0);

        for (GraphEdge const& edge : g.edges)
        {
            if (edge.source.node == GRAPH_ID) continue;
            if (edge.target.node == GRAPH_ID) continue;

            size_t const u = edge.source.node;
            size_t const v = edge.target.node;

            if (outgoing[u].insert(v).second) {
                ++indegree[v];
            }
        }

        for (GraphEventEdge const& edge : g.event_edges)
        {
            if (edge.source.node == GRAPH_ID) continue;
            if (edge.target.node == GRAPH_ID) continue;

            size_t const u = edge.source.node;
            size_t const v = edge.target.node;

            if (outgoing[u].insert(v).second) {
                ++indegree[v];
            }
        }

        return std::make_pair(std::move(outgoing), std::move(indegree));
    }

    constexpr void apply_node_permutation(ExecutableGraphData& g, std::vector<size_t> const& sorted)
    {
        size_t const num_nodes = g.nodes.size();

        std::vector<ReflectedNodeDescription> sorted_nodes;
        std::vector<std::optional<size_t>> sorted_explicit_ttls;
        std::vector<std::string> sorted_node_ids;
        std::vector<std::vector<std::string>> sorted_node_virtual_ids;
        std::vector<std::vector<SourceInfo>> sorted_node_source_infos;
        std::vector<size_t> sorted_node_construction_order;
        sorted_nodes.reserve(num_nodes);
        sorted_explicit_ttls.reserve(num_nodes);
        sorted_node_ids.reserve(num_nodes);
        sorted_node_virtual_ids.reserve(num_nodes);
        sorted_node_source_infos.reserve(num_nodes);
        sorted_node_construction_order.reserve(num_nodes);
        for (size_t old_i = 0; old_i < num_nodes; ++old_i)
        {
            sorted_nodes.push_back(std::move(g.nodes[sorted[old_i]]));
            sorted_explicit_ttls.push_back(std::move(g.explicit_ttl_samples[sorted[old_i]]));
            sorted_node_ids.push_back(std::move(g.node_ids[sorted[old_i]]));
            sorted_node_virtual_ids.push_back(std::move(g.node_virtual_ids[sorted[old_i]]));
            sorted_node_source_infos.push_back(std::move(g.node_source_infos[sorted[old_i]]));
            sorted_node_construction_order.push_back(g.node_construction_order[sorted[old_i]]);
        }
        g.nodes.swap(sorted_nodes);
        g.explicit_ttl_samples.swap(sorted_explicit_ttls);
        g.node_ids.swap(sorted_node_ids);
        g.node_virtual_ids.swap(sorted_node_virtual_ids);
        g.node_source_infos.swap(sorted_node_source_infos);
        g.node_construction_order.swap(sorted_node_construction_order);

        std::vector<size_t> reverse_sorted(num_nodes);
        for (size_t new_i = 0; new_i < num_nodes; ++new_i) {
            reverse_sorted[sorted[new_i]] = new_i;
        }

        std::flat_set<GraphEdge> sorted_edges;
        for (GraphEdge edge : g.edges)
        {
            if (edge.source.node != GRAPH_ID)
                edge.source.node = reverse_sorted[edge.source.node];
            if (edge.target.node != GRAPH_ID)
                edge.target.node = reverse_sorted[edge.target.node];
            sorted_edges.insert(edge);
        }
        g.edges.swap(sorted_edges);

        std::flat_set<GraphEventEdge> sorted_event_edges;
        for (GraphEventEdge edge : g.event_edges)
        {
            if (edge.source.node != GRAPH_ID)
                edge.source.node = reverse_sorted[edge.source.node];
            if (edge.target.node != GRAPH_ID)
                edge.target.node = reverse_sorted[edge.target.node];
            sorted_event_edges.insert(std::move(edge));
        }
        g.event_edges.swap(sorted_event_edges);

        for (auto&& [_, info] : g.detached_info_by_source)
        {
            if (info.original_source.node != GRAPH_ID) {
                info.original_source.node = reverse_sorted[info.original_source.node];
            }
            if (info.writer_node != GRAPH_ID) {
                info.writer_node = reverse_sorted[info.writer_node];
            }
            if (info.reader_output.node != GRAPH_ID) {
                info.reader_output.node = reverse_sorted[info.reader_output.node];
            }
        }
    }

    constexpr void sort_nodes_or_error(ExecutableGraphData& g, std::string_view builder_id)
    {
        auto [outgoing, indegree] = make_node_adjacency(g);

        std::vector<size_t> ready;
        for (size_t node = 0; node < indegree.size(); ++node) {
            if (indegree[node] == 0) {
                ready.push_back(node);
            }
        }

        std::vector<size_t> sorted;
        sorted.reserve(g.nodes.size());

        size_t ready_index = 0;
        while (ready_index < ready.size())
        {
            size_t const node = ready[ready_index++];
            sorted.push_back(node);

            for (size_t target : outgoing[node])
            {
                if (--indegree[target] == 0) {
                    ready.push_back(target);
                }
            }
        }

        if (sorted.size() != g.nodes.size()) {
            error(
                "builder " + std::string(builder_id) + ": graph contains a cycle; use detach() to break feedback explicitly"
            );
        }

        apply_node_permutation(g, sorted);
    }


    constexpr GraphExecutionPlan build_execution_plan(
        std::vector<ReflectedNodeDescription> const& nodes,
        std::flat_set<GraphEdge> const& edges,
        std::flat_set<GraphEventEdge> const& event_edges,
        std::vector<DetachedInfo> const& detached
    )
    {
        size_t const num_nodes = nodes.size();
        GraphExecutionPlan plan;
        plan.node_to_region.assign(num_nodes, 0);
        if (num_nodes == 0) {
            return plan;
        }

        std::vector<std::vector<size_t>> outgoing(num_nodes);
        std::flat_map<ConcretePortId, std::vector<ConcretePortId>> consumers;
        for (auto const& edge : edges) {
            consumers[edge.source].push_back(edge.target);
            if (edge.source.node == GRAPH_ID || edge.target.node == GRAPH_ID) {
                continue;
            }
            outgoing[edge.source.node].push_back(edge.target.node);
        }

        for (auto const& edge : event_edges) {
            if (edge.source.node == GRAPH_ID || edge.target.node == GRAPH_ID) {
                continue;
            }
            outgoing[edge.source.node].push_back(edge.target.node);
        }

        for (auto const& detached_info : detached) {
            if (detached_info.writer_node == GRAPH_ID || detached_info.reader_output.node == GRAPH_ID) {
                continue;
            }
            outgoing[detached_info.writer_node].push_back(detached_info.reader_output.node);
        }

        std::vector<size_t> index(num_nodes, std::numeric_limits<size_t>::max());
        std::vector<size_t> lowlink(num_nodes, 0);
        std::vector<bool> on_stack(num_nodes, false);
        std::vector<size_t> stack;
        size_t next_index = 0;
        std::vector<std::vector<size_t>> sccs;

        auto strongconnect = [&](auto&& self, size_t v) -> void {
            index[v] = next_index;
            lowlink[v] = next_index;
            ++next_index;
            stack.push_back(v);
            on_stack[v] = true;

            for (size_t w : outgoing[v]) {
                if (index[w] == std::numeric_limits<size_t>::max()) {
                    self(self, w);
                    lowlink[v] = std::min(lowlink[v], lowlink[w]);
                } else if (on_stack[w]) {
                    lowlink[v] = std::min(lowlink[v], index[w]);
                }
            }

            if (lowlink[v] == index[v]) {
                auto& scc = sccs.emplace_back();
                while (true) {
                    size_t w = stack.back();
                    stack.pop_back();
                    on_stack[w] = false;
                    scc.push_back(w);
                    if (w == v) {
                        break;
                    }
                }
            }
        };

        for (size_t node = 0; node < num_nodes; ++node) {
            if (index[node] == std::numeric_limits<size_t>::max()) {
                strongconnect(strongconnect, node);
            }
        }

        plan.regions.reserve(sccs.size());
        for (auto& scc : sccs) {
            std::sort(scc.begin(), scc.end());
            GraphRegion region;
            region.nodes = scc;
            region.execution_order = scc;
            region.max_block_size = MAX_BLOCK_SIZE;
            for (size_t node : scc) {
                plan.node_to_region[node] = plan.regions.size();
                region.max_block_size = std::min(region.max_block_size, nodes[node].max_block_size());
            }
            plan.regions.push_back(std::move(region));
        }

        for (auto const& detached_info : detached) {
            if (detached_info.original_source.node == GRAPH_ID) {
                continue;
            }
            auto it = consumers.find(detached_info.reader_output);
            if (it == consumers.end()) {
                continue;
            }
            for (ConcretePortId consumer : it->second) {
                if (consumer.node == GRAPH_ID) {
                    continue;
                }
                size_t const source_region = plan.node_to_region[detached_info.original_source.node];
                size_t const consumer_region = plan.node_to_region[consumer.node];
                if (source_region == consumer_region) {
                    plan.regions[source_region].max_block_size = std::min(
                        plan.regions[source_region].max_block_size,
                        floor_power_of_2(detached_info.loop_extra_latency)
                    );
                }
            }
        }

        // Construct the explicit in-region graph once. The previous form
        // rescanned every edge for every SCC and rebuilt flat maps/sets for
        // each one. Sorting each source's vector preserves the deterministic
        // target order previously supplied by flat_set.
        std::vector<std::vector<size_t>> internal_outgoing(num_nodes);
        auto append_internal_edge = [&](ConcretePortId source,
                                        ConcretePortId target) {
            if (source.node == GRAPH_ID || target.node == GRAPH_ID) {
                return;
            }
            if (plan.node_to_region[source.node]
                != plan.node_to_region[target.node]) {
                return;
            }
            internal_outgoing[source.node].push_back(target.node);
        };
        for (auto const& edge : edges) {
            append_internal_edge(edge.source, edge.target);
        }
        for (auto const& edge : event_edges) {
            append_internal_edge(edge.source, edge.target);
        }
        for (auto& targets : internal_outgoing) {
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()),
                          targets.end());
        }

        std::vector<size_t> local_indegree(num_nodes, 0);
        for (GraphRegion& region : plan.regions) {
            for (size_t node : region.nodes) {
                local_indegree[node] = 0;
            }
            for (size_t node : region.nodes) {
                for (size_t target : internal_outgoing[node]) {
                    ++local_indegree[target];
                }
            }

            std::vector<size_t> ready;
            for (size_t node : region.nodes) {
                if (local_indegree[node] == 0) {
                    ready.push_back(node);
                }
            }
            std::sort(ready.begin(), ready.end());

            region.execution_order.clear();
            region.execution_order.reserve(region.nodes.size());
            while (!ready.empty()) {
                size_t const node = ready.front();
                ready.erase(ready.begin());
                region.execution_order.push_back(node);

                for (size_t target : internal_outgoing[node]) {
                    if (--local_indegree[target] == 0) {
                        ready.insert(
                            std::lower_bound(ready.begin(), ready.end(), target),
                            target
                        );
                    }
                }
            }

            if (region.execution_order.size() != region.nodes.size()) {
                error("graph region remains cyclic after detached edge removal");
            }
        }

        // Likewise, materialize the SCC DAG once rather than maintaining a
        // flat set for every region while scanning the same edge sets again.
        std::vector<std::vector<size_t>> region_outgoing(plan.regions.size());
        std::vector<size_t> indegree(plan.regions.size(), 0);
        auto append_region_edge = [&](ConcretePortId source,
                                      ConcretePortId target) {
            if (source.node == GRAPH_ID || target.node == GRAPH_ID) {
                return;
            }
            size_t const source_region = plan.node_to_region[source.node];
            size_t const target_region = plan.node_to_region[target.node];
            if (source_region != target_region) {
                region_outgoing[source_region].push_back(target_region);
            }
        };
        for (auto const& edge : edges) {
            append_region_edge(edge.source, edge.target);
        }
        for (auto const& edge : event_edges) {
            append_region_edge(edge.source, edge.target);
        }
        for (auto& targets : region_outgoing) {
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()),
                          targets.end());
            for (size_t target : targets) {
                ++indegree[target];
            }
        }

        std::vector<size_t> ready;
        for (size_t region = 0; region < indegree.size(); ++region) {
            if (indegree[region] == 0) {
                ready.push_back(region);
            }
        }

        size_t ready_index = 0;
        while (ready_index < ready.size()) {
            size_t region = ready[ready_index++];
            plan.region_order.push_back(region);
            for (size_t target : region_outgoing[region]) {
                if (--indegree[target] == 0) {
                    ready.push_back(target);
                }
            }
        }

        return plan;
    }

    constexpr auto make_node_configs(
        auto const& node,
        size_t node_i,
        std::span<InputConfig const> graph_private_inputs
    )
    {
        std::vector<InputConfig> input_configs;
        std::vector<OutputConfig> output_configs;

        if (node_i == GRAPH_ID) {
            input_configs.assign(graph_private_inputs.begin(), graph_private_inputs.end());
        } else {
            auto const inputs = get_inputs(node);
            auto const outputs = get_outputs(node);
            input_configs.assign(inputs.begin(), inputs.end());
            output_configs.assign(outputs.begin(), outputs.end());
        }

        return std::make_pair(std::move(input_configs), std::move(output_configs));
    }

    constexpr size_t connection_block_size(
        ConcretePortId source,
        ConcretePortId target,
        size_t host_block_size,
        GraphExecutionPlan const& plan
    )
    {
        auto region_block = [&](size_t node) {
            if (node == GRAPH_ID) {
                return host_block_size;
            }
            return std::min(host_block_size, plan.regions[plan.node_to_region[node]].max_block_size);
        };
        return std::max(region_block(source.node), region_block(target.node));
    }

    constexpr size_t region_internal_latency(
        GraphRegion const& region,
        std::vector<ReflectedNodeDescription> const& nodes,
        std::flat_set<GraphEdge> const& edges
    )
    {
        std::flat_set<size_t> region_nodes(region.nodes.begin(), region.nodes.end());
        std::flat_map<ConcretePortId, std::vector<ConcretePortId>> targets_of;
        for (GraphEdge const& edge : edges) {
            if (
                edge.source.node == GRAPH_ID
                || edge.target.node == GRAPH_ID
                || !region_nodes.contains(edge.source.node)
                || !region_nodes.contains(edge.target.node)
            ) {
                continue;
            }
            targets_of[edge.source].push_back(edge.target);
        }

        std::flat_map<ConcretePortId, size_t> input_latencies;
        size_t max_latency = 0;
        for (size_t const node_i : region.execution_order) {
            size_t node_latency = 0;
            auto const inputs = nodes[node_i].inputs();
            for (size_t input_port = 0; input_port < inputs.size(); ++input_port) {
                node_latency = std::max(node_latency, input_latencies[{ node_i, input_port }]);
            }

            node_latency += nodes[node_i].internal_latency();
            max_latency = std::max(max_latency, node_latency);

            auto const outputs = nodes[node_i].outputs();
            for (size_t output_port = 0; output_port < outputs.size(); ++output_port) {
                size_t const output_latency = node_latency + outputs[output_port].latency;
                max_latency = std::max(max_latency, output_latency);
                if (auto it = targets_of.find({ node_i, output_port }); it != targets_of.end()) {
                    for (ConcretePortId const target : it->second) {
                        input_latencies[target] = output_latency;
                    }
                }
            }
        }
        return max_latency;
    }

    enum class GraphArtifactWrapperBuildMode {
        none,
        nodes,
        sccs,
    };

    consteval GraphBuildArtifact build_graph_artifact(
        std::string graph_id,
        std::vector<ReflectedNodeDescription> nodes,
        std::vector<std::optional<size_t>> explicit_ttl_samples,
        std::vector<std::string> node_ids,
        std::flat_set<GraphEdge> edges,
        std::flat_set<GraphEventEdge> event_edges,
        std::vector<DetachedInfo> detached,
        GraphExecutionPlan execution_plan,
        std::vector<InputConfig> public_inputs,
        std::vector<OutputConfig> public_outputs,
        std::vector<EventInputConfig> public_event_inputs,
        std::vector<EventOutputConfig> public_event_outputs,
        std::vector<DormancyGroup> dormancy_groups,
        // Diagnostic callers can isolate edge/buffer/latency preparation,
        // node wrapper construction, and SCC wrapper construction.
        GraphArtifactWrapperBuildMode wrapper_build_mode =
            GraphArtifactWrapperBuildMode::sccs,
        GraphNodeWrapperBuildMode node_wrapper_build_mode =
            GraphNodeWrapperBuildMode::full
    )
    {
        std::flat_set<GraphEdge> resolved_edges;
        auto output_layout_for = [&](ConcretePortId port) {
            return port.node == GRAPH_ID
                ? effective_channel_layout(public_inputs[port.port])
                : effective_channel_layout(nodes[port.node].outputs()[port.port]);
        };
        auto input_layout_for = [&](ConcretePortId port) {
            return port.node == GRAPH_ID
                ? effective_channel_layout(public_outputs[port.port])
                : effective_channel_layout(nodes[port.node].inputs()[port.port]);
        };
        for (GraphEdge const& edge : edges) {
            resolved_edges.emplace(
                edge.source,
                edge.target,
                ChannelConversionRegistry::plan(output_layout_for(edge.source), input_layout_for(edge.target))
            );
        }
        edges = std::move(resolved_edges);

        auto [source_of, targets_of] = [&] {
            std::flat_map<ConcretePortId, ConcretePortId> source_of_;
            std::flat_map<ConcretePortId, std::vector<ConcretePortId>> targets_of_;
            for (GraphEdge const& edge : edges) {
                source_of_[edge.target] = edge.source;
                targets_of_[edge.source].push_back(edge.target);
            }
            return std::make_tuple(std::move(source_of_), std::move(targets_of_));
        }();

        std::vector<InputConfig> private_input_configs;
        private_input_configs.reserve(public_outputs.size());
        for (auto const& output : public_outputs) {
            private_input_configs.push_back(InputConfig{
                .name = output.name,
                .channel_layout = output.channel_layout,
            });
        }
        std::flat_map<ConcretePortId, size_t> input_port_global_latencies;
        auto align_latencies = [&](auto const& node, size_t node_i,
                                   std::span<InputConfig const> input_configs,
                                   std::span<OutputConfig const> output_configs) {
            size_t node_global_latency = 0;
            for (size_t input = 0; input < input_configs.size(); ++input)
                node_global_latency = std::max(
                    node_global_latency,
                    input_port_global_latencies[{node_i, input}]);
            if (node_i == GRAPH_ID) return;
            node_global_latency += get_internal_latency(node);
            for (size_t output = 0; output < output_configs.size(); ++output) {
                if (auto it = targets_of.find({node_i, output}); it != targets_of.end()) {
                    for (ConcretePortId const target : it->second) {
                        input_port_global_latencies[target] =
                            node_global_latency + output_configs[output].latency;
                    }
                }
            }
        };
        auto delay_input = [&](ConcretePortId input, size_t extra_delay) {
            if (input.node == GRAPH_ID)
                return input_port_global_latencies[input] += extra_delay;
            return input_port_global_latencies.at(input) += extra_delay;
        };
        std::vector<std::vector<PortBufferPlan>> node_input_buffer_plans(nodes.size());
        std::vector<PortBufferPlan> public_output_buffer_plans(public_outputs.size());

        for (size_t node_i = 0; node_i < nodes.size() + 1; ++node_i) {
            if (node_i < nodes.size()) {
                auto [input_configs, output_configs] = make_node_configs(nodes[node_i], node_i, private_input_configs);
                align_latencies(nodes[node_i], node_i, input_configs, output_configs);

                for (size_t input_i = 0; input_i < input_configs.size(); ++input_i) {
                    ConcretePortId const this_input { node_i, input_i };

                    if (auto it = source_of.find(this_input); it != source_of.end()) {
                        size_t const output_node_i = it->second.node;
                        size_t const output_port_i = it->second.port;
                        OutputConfig const output_config = (output_node_i == GRAPH_ID)
                            ? OutputConfig{
                                .name = public_inputs[output_port_i].name,
                                .channel_layout = public_inputs[output_port_i].channel_layout,
                            }
                            : nodes[output_node_i].outputs()[output_port_i];
                        size_t const corrected_latency = delay_input(this_input, output_config.latency);
                        node_input_buffer_plans[node_i].push_back({
                            .connection_max_block_size = connection_block_size(it->second, this_input, MAX_BLOCK_SIZE, execution_plan),
                            .corrected_latency = corrected_latency,
                            .input_history = input_configs[input_i].history,
                            .output_history = output_config.history,
                        });
                    } else {
                        node_input_buffer_plans[node_i].push_back({
                            .connection_max_block_size = MAX_BLOCK_SIZE,
                            .corrected_latency = 0,
                            .input_history = input_configs[input_i].history,
                            .output_history = 0,
                        });
                    }
                }
            } else {
                std::vector<InputConfig> input_configs = private_input_configs;

                for (size_t input_i = 0; input_i < input_configs.size(); ++input_i) {
                    ConcretePortId const this_input { GRAPH_ID, input_i };
                    if (auto it = source_of.find(this_input); it != source_of.end()) {
                        size_t const output_node_i = it->second.node;
                        size_t const output_port_i = it->second.port;
                        OutputConfig const output_config = (output_node_i == GRAPH_ID)
                            ? OutputConfig{
                                .name = public_inputs[output_port_i].name,
                                .channel_layout = public_inputs[output_port_i].channel_layout,
                            }
                            : nodes[output_node_i].outputs()[output_port_i];
                        size_t const corrected_latency = delay_input(this_input, output_config.latency);
                        public_output_buffer_plans[input_i] = {
                            .connection_max_block_size = connection_block_size(it->second, this_input, MAX_BLOCK_SIZE, execution_plan),
                            .corrected_latency = corrected_latency,
                            .input_history = input_configs[input_i].history,
                            .output_history = output_config.history,
                        };
                    } else {
                        public_output_buffer_plans[input_i] = {
                            .connection_max_block_size = MAX_BLOCK_SIZE,
                            .corrected_latency = 0,
                            .input_history = input_configs[input_i].history,
                            .output_history = 0,
                        };
                    }
                }
            }
        }

        GraphBuildArtifact artifact {
            .graph_id = std::move(graph_id),
            .scc_wrappers = {},
            .edges = std::move(edges),
            .event_edges = std::move(event_edges),
            .detached = std::move(detached),
            .execution_plan = std::move(execution_plan),
            .public_inputs = std::move(public_inputs),
            .public_outputs = std::move(public_outputs),
            .public_event_inputs = std::move(public_event_inputs),
            .public_event_outputs = std::move(public_event_outputs),
            .public_output_buffer_plans = std::move(public_output_buffer_plans),
            .public_output_bindings = {},
            .public_input_fanout_storage = {},
            .public_input_targets = {},
            .dormancy_groups = std::move(dormancy_groups),
            .internal_latency = 0,
            .node_ids = std::move(node_ids),
        };
        // Sample fan-out stays at the buffer boundary.  Identity-layout
        // consumers alias one owner; only layout-changing branches receive a
        // wrapper-managed copy from that owner.  This replaces synthetic
        // Broadcast nodes without conflating consumer cursors or latencies.
        std::vector<std::vector<SampleInputBinding>> node_input_bindings(
            nodes.size());
        std::vector<std::vector<std::vector<SampleOutputBinding>>>
            node_output_targets(nodes.size());
        std::vector<std::vector<SampleBufferStorage>>
            node_output_fanout_storage(nodes.size());
        for (size_t node_i = 0; node_i < nodes.size(); ++node_i) {
            node_input_bindings[node_i].resize(
                node_input_buffer_plans[node_i].size());
            node_output_targets[node_i].resize(nodes[node_i].outputs().size());
        }
        artifact.public_output_bindings.resize(
            artifact.public_output_buffer_plans.size());
        artifact.public_input_targets.resize(artifact.public_inputs.size());

        auto target_export_id = [&](ConcretePortId target) {
            return target.node == GRAPH_ID
                ? graph_port_data_export_id(artifact.graph_id, target.port)
                : port_data_export_id(artifact.node_ids[target.node], target.port);
        };
        auto source_storage_id = [&](ConcretePortId source) {
            return source.node == GRAPH_ID
                ? graph_source_port_data_export_id(artifact.graph_id, source.port)
                : source_port_data_export_id(artifact.node_ids[source.node], source.port);
        };
        auto target_binding = [&](ConcretePortId target) -> SampleInputBinding& {
            return target.node == GRAPH_ID
                ? artifact.public_output_bindings[target.port]
                : node_input_bindings[target.node][target.port];
        };
        auto target_plan = [&](ConcretePortId target) -> PortBufferPlan& {
            return target.node == GRAPH_ID
                ? artifact.public_output_buffer_plans[target.port]
                : node_input_buffer_plans[target.node][target.port];
        };
        auto source_input_config = [&](ConcretePortId source) {
            if (source.node == GRAPH_ID) {
                return artifact.public_inputs[source.port];
            }
            auto const output = nodes[source.node].outputs()[source.port];
            return InputConfig{
                .name = output.name,
                .channel_layout = output.channel_layout,
            };
        };
        auto merge_buffer_plan = [](PortBufferPlan& destination,
                                    PortBufferPlan const& source) {
            destination.connection_max_block_size = std::max(
                destination.connection_max_block_size,
                source.connection_max_block_size);
            destination.corrected_latency = std::max(
                destination.corrected_latency, source.corrected_latency);
            destination.input_history = std::max(
                destination.input_history, source.input_history);
            destination.output_history = std::max(
                destination.output_history, source.output_history);
        };
        std::flat_map<ConcretePortId, std::vector<GraphEdge>> sample_targets;
        for (GraphEdge const& edge : artifact.edges) {
            sample_targets[edge.source].push_back(edge);
        }
        for (auto const& [source, targets] : sample_targets) {
            std::vector<GraphEdge const*> direct_targets;
            for (GraphEdge const& edge : targets) {
                if (edge.conversion.source == edge.conversion.target) {
                    direct_targets.push_back(&edge);
                }
            }

            std::vector<SampleOutputBinding> source_targets;
            if (!direct_targets.empty()) {
                GraphEdge const& owner_edge = *direct_targets.front();
                ConcretePortId const owner_target = owner_edge.target;
                source_targets.push_back({
                    .target = target_export_id(owner_target),
                    .conversion = owner_edge.conversion,
                });

                for (GraphEdge const* edge : direct_targets) {
                    merge_buffer_plan(target_plan(owner_target),
                                      target_plan(edge->target));
                    if (edge->target == owner_target) {
                        continue;
                    }
                    auto& binding = target_binding(edge->target);
                    binding.owns_storage = false;
                    target_binding(owner_target).aliases.push_back(
                        target_export_id(edge->target));
                }
                for (GraphEdge const& edge : targets) {
                    if (edge.conversion.source != edge.conversion.target) {
                        source_targets.push_back({
                            .target = target_export_id(edge.target),
                            .conversion = edge.conversion,
                        });
                    }
                }
            } else {
                PortBufferPlan storage_plan = target_plan(targets.front().target);
                for (size_t target_i = 1; target_i < targets.size(); ++target_i) {
                    merge_buffer_plan(storage_plan,
                                      target_plan(targets[target_i].target));
                }
                auto const storage_id = source_storage_id(source);
                SampleBufferStorage storage{
                    .id = storage_id,
                    .config = source_input_config(source),
                    .plan = storage_plan,
                };
                if (source.node == GRAPH_ID) {
                    artifact.public_input_fanout_storage.push_back(
                        std::move(storage));
                } else {
                    node_output_fanout_storage[source.node].push_back(
                        std::move(storage));
                }
                source_targets.push_back({
                    .target = storage_id,
                    .conversion = ChannelConversionRegistry::plan(
                        output_layout_for(source), output_layout_for(source)),
                });
                for (GraphEdge const& edge : targets) {
                    source_targets.push_back({
                        .target = target_export_id(edge.target),
                        .conversion = edge.conversion,
                    });
                }
            }

            if (source.node == GRAPH_ID) {
                artifact.public_input_targets[source.port] =
                    std::move(source_targets);
            } else {
                node_output_targets[source.node][source.port] =
                    std::move(source_targets);
            }
        }
        // Dormancy probes are independent readers too.  Preserve the same
        // consumer-side latency that the corresponding InputPort uses.
        std::flat_map<std::string, size_t> latency_by_port_data_id;
        for (size_t node_i = 0; node_i < nodes.size(); ++node_i) {
            for (size_t input_i = 0;
                 input_i < node_input_buffer_plans[node_i].size();
                 ++input_i) {
                latency_by_port_data_id.emplace(
                    port_data_export_id(artifact.node_ids[node_i], input_i),
                    node_input_buffer_plans[node_i][input_i].corrected_latency);
            }
        }
        for (size_t output_i = 0;
             output_i < artifact.public_output_buffer_plans.size();
             ++output_i) {
            latency_by_port_data_id.emplace(
                graph_port_data_export_id(artifact.graph_id, output_i),
                artifact.public_output_buffer_plans[output_i].corrected_latency);
        }
        for (DormancyGroup& group : artifact.dormancy_groups) {
            auto set_frontier_latencies = [&](auto& frontier) {
                for (DormancySamplePort& port : frontier) {
                    if (auto it = latency_by_port_data_id.find(port.export_id);
                        it != latency_by_port_data_id.end()) {
                        port.latency_samples = it->second;
                    }
                }
            };
            set_frontier_latencies(group.sample_input_frontier);
            set_frontier_latencies(group.sample_output_frontier);
        }
        {
            std::flat_map<ConcretePortId, std::vector<ConcretePortId>> artifact_targets_of;
            for (GraphEdge const& edge : artifact.edges) {
                artifact_targets_of[edge.source].push_back(edge.target);
            }

            std::flat_map<ConcretePortId, size_t> input_global_latencies;
            size_t max_latency = 0;

            auto process_node = [&](
                ReflectedNodeDescription const& node, size_t node_i) {
                size_t node_global_latency = 0;
                auto node_inputs = node.inputs();
                for (size_t input_port = 0; input_port < node_inputs.size(); ++input_port) {
                    node_global_latency = std::max(node_global_latency, input_global_latencies[{ node_i, input_port }]);
                }

                node_global_latency += node.internal_latency();
                auto node_outputs = node.outputs();
                for (size_t output_port = 0; output_port < node_outputs.size(); ++output_port) {
                    if (auto it = artifact_targets_of.find({ node_i, output_port }); it != artifact_targets_of.end()) {
                        size_t const new_latency = node_global_latency + node_outputs[output_port].latency;
                        max_latency = std::max(max_latency, new_latency);
                        for (ConcretePortId const target : it->second) {
                            input_global_latencies[target] = new_latency;
                        }
                    }
                }
            };

            for (size_t node_i = 0; node_i < nodes.size(); ++node_i) {
                process_node(nodes[node_i], node_i);
            }

            size_t graph_global_latency = 0;
            for (size_t input_port = 0; input_port < artifact.public_outputs.size(); ++input_port) {
                graph_global_latency = std::max(graph_global_latency, input_global_latencies[{ GRAPH_ID, input_port }]);
            }
            artifact.internal_latency = std::max(max_latency, graph_global_latency);
        }

        if (wrapper_build_mode == GraphArtifactWrapperBuildMode::none) {
            return artifact;
        }

        if (wrapper_build_mode == GraphArtifactWrapperBuildMode::sccs) {
            artifact.scc_wrappers.reserve(
                artifact.execution_plan.region_order.size());
        }
        for (size_t ordered_scc_i = 0; ordered_scc_i < artifact.execution_plan.region_order.size(); ++ordered_scc_i) {
            size_t const region_i = artifact.execution_plan.region_order[ordered_scc_i];
            auto const& region = artifact.execution_plan.regions[region_i];
            std::vector<GraphNodeWrapper> region_nodes;
            std::vector<size_t> region_global_node_indices;
            region_nodes.reserve(region.execution_order.size());
            region_global_node_indices.reserve(region.execution_order.size());

            for (size_t global_i : region.execution_order) {
                std::vector<EventOutputBinding> event_output_targets;
                auto event_inputs = nodes[global_i].event_inputs();
                auto event_outputs = nodes[global_i].event_outputs();
                event_output_targets.reserve(event_outputs.size());
                for (size_t output_i = 0; output_i < event_outputs.size(); ++output_i) {
                    auto it = std::find_if(
                        artifact.event_edges.begin(),
                        artifact.event_edges.end(),
                        [&](GraphEventEdge const& edge) {
                            return edge.source == ConcretePortId{ global_i, output_i };
                        }
                    );
                    if (it != artifact.event_edges.end()) {
                        if (it->target.node == GRAPH_ID) {
                            event_output_targets.push_back(EventOutputBinding{
                                .target = graph_event_port_data_export_id(
                                    artifact.graph_id,
                                    it->target.port
                                ),
                                .conversion = it->conversion,
                            });
                        } else {
                            event_output_targets.push_back(EventOutputBinding{
                                .target = event_port_data_export_id(
                                    artifact.node_ids[it->target.node],
                                    it->target.port
                                ),
                                .conversion = it->conversion,
                            });
                        }
                    } else {
                        event_output_targets.push_back(EventOutputBinding{});
                    }
                }
                region_nodes.emplace_back(
                    std::move(nodes[global_i]),
                    explicit_ttl_samples[global_i],
                    std::move(node_input_buffer_plans[global_i]),
                    std::move(node_input_bindings[global_i]),
                    std::vector<EventInputConfig>(event_inputs.begin(), event_inputs.end()),
                    artifact.node_ids[global_i],
                    std::move(node_output_targets[global_i]),
                    std::move(node_output_fanout_storage[global_i]),
                    std::move(event_output_targets),
                    node_wrapper_build_mode
                );
                region_global_node_indices.push_back(global_i);
            }

            size_t const internal_latency = region_internal_latency(region, nodes, artifact.edges);

            if (wrapper_build_mode == GraphArtifactWrapperBuildMode::sccs) {
                artifact.scc_wrappers.emplace_back(
                    std::move(region_nodes),
                    std::move(region_global_node_indices),
                    region.max_block_size,
                    internal_latency,
                    region.nodes.size() > 1 ? region.max_block_size : 0,
                    artifact.dormancy_groups.empty()
                        ? std::string{}
                        : graph_dormancy_node_skip_export_id(
                              artifact.graph_id));
            }
        }

        return artifact;
    }

    constexpr std::flat_map<std::string, std::vector<std::string>>
    build_virtual_node_ids_by_backing_node_id(
        ExecutableGraphData const& g,
        std::span<LoweredSubgraphSpec const> scopes) {
        std::flat_map<std::string, std::vector<std::string>> result;
        for (size_t node_i = 0; node_i < g.nodes.size(); ++node_i) {
            if (node_i >= g.node_virtual_ids.size()) continue;
            for (auto const& virtual_node_id : g.node_virtual_ids[node_i])
                result[g.node_ids[node_i]].push_back(virtual_node_id);
        }
        for (auto const& scope : scopes) {
            auto const type_identity = "lowered-subgraph:" + scope.kind;
            for (auto const& info : scope.source_infos) {
                if (!info.declaration_identity.empty())
                    result[scope.backing_node_id].push_back(
                        typed_virtual_node_id(info.declaration_identity,
                                              type_identity));
            }
        }
        for (auto&& [_, ids] : result) {
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        }
        return result;
    }
}

namespace iv {
struct CompiledGraph {
    Graph graph;
    GraphBuildMetadata metadata;
    GraphIntrospectionMetadata introspection;
};

class GraphCompiler {
public:
    static consteval CompiledGraph compile(ExecutableGraphIR executable) {
        details::sort_nodes_or_error(executable.graph, executable.graph_id);
        for (auto const& edge : executable.graph.edges) {
            auto const source_count = edge.source.node == GRAPH_ID
                ? executable.public_inputs.size()
                : get_num_outputs(executable.graph.nodes[edge.source.node]);
            auto const target_count = edge.target.node == GRAPH_ID
                ? executable.public_outputs.size()
                : get_num_inputs(executable.graph.nodes[edge.target.node]);
            if (edge.source.port >= source_count || edge.target.port >= target_count)
                details::error("executable IR has an out-of-range sample edge");
        }
        auto lowered_subgraphs = details::compile_lowered_subgraphs(
            executable.graph, executable.scopes);
        auto virtual_by_backing =
            details::build_virtual_node_ids_by_backing_node_id(
                executable.graph, executable.scopes);
        std::vector<DetachedInfo> detached;
        detached.reserve(executable.graph.detached_info_by_source.size());
        for (auto const& [_, info] : executable.graph.detached_info_by_source)
            detached.push_back(info);
        auto execution_plan = details::build_execution_plan(
            executable.graph.nodes, executable.graph.edges,
            executable.graph.event_edges, detached);
        auto dormancy_groups = details::compile_dormancy_groups(
            executable.graph, lowered_subgraphs, executable.graph_id,
            execution_plan);
        auto node_source_infos = std::move(executable.graph.node_source_infos);
        auto node_type_identities =
            std::move(executable.graph.node_type_identities);
        return {
            .graph = Graph(details::build_graph_artifact(
                executable.graph_id, std::move(executable.graph.nodes),
                std::move(executable.graph.explicit_ttl_samples),
                std::move(executable.graph.node_ids),
                std::move(executable.graph.edges),
                std::move(executable.graph.event_edges), std::move(detached),
                std::move(execution_plan),
                std::move(executable.public_inputs),
                std::move(executable.public_outputs),
                std::move(executable.public_event_inputs),
                std::move(executable.public_event_outputs),
                std::move(dormancy_groups))),
            .metadata = {
                .lowered_subgraphs = std::move(lowered_subgraphs),
                .concrete_node_type_identities = std::move(node_type_identities),
                .node_source_infos = std::move(node_source_infos),
                .virtual_node_ids_by_backing_node_id =
                    std::move(virtual_by_backing),
            },
            .introspection = std::move(executable.introspection),
        };
    }
};
} // namespace iv
