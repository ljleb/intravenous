#pragma once

#include "basic_nodes/routing.h"
#include "basic_nodes/arithmetic.h"
#include "basic_nodes/type_erased.h"
#include "graph/build_types.h"
#include "graph/types.h"
#include "graph/wiring.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iv::details {
    [[noreturn]] inline void error(std::string msg)
    {
        throw std::logic_error(msg);
    }

    struct PreparedGraph {
        std::vector<TypeErasedNode> nodes;
        std::vector<std::string> node_ids;
        std::unordered_set<GraphEdge> edges;
        std::unordered_map<PortId, DetachedInfo> detached_info_by_source;
        std::unordered_set<PortId> detached_reader_outputs;
    };

    inline std::string generated_node_id(std::string_view builder_id, size_t generated_index)
    {
        std::string id(builder_id);
        if (!id.empty()) {
            id += ".";
        }
        id += "generated.";
        id += std::to_string(generated_index);
        return id;
    }

    inline auto make_source_target_edge_maps(PreparedGraph const& g)
    {
        std::unordered_map<PortId, PortId> source_of;
        std::unordered_map<PortId, PortId> target_of;

        for (GraphEdge const& edge : g.edges)
        {
            source_of[edge.target] = edge.source;
            target_of[edge.source] = edge.target;
        }

        return std::make_tuple(std::move(source_of), std::move(target_of));
    }

    inline void expand_hyperedge_ports(PreparedGraph& g, std::string_view builder_id)
    {
        std::unordered_map<PortId, std::vector<GraphEdge>> reverse_edges_map;
        for (GraphEdge const& edge : g.edges)
        {
            reverse_edges_map[edge.target].push_back(edge);
        }

        size_t nodes_size = g.nodes.size();
        for (size_t node = 0; node < nodes_size; ++node)
        {
            size_t const num_inputs = get_num_inputs(g.nodes[node]);
            for (size_t in_port = 0; in_port < num_inputs; ++in_port)
            {
                auto it = reverse_edges_map.find({ node, in_port });
                if (it == reverse_edges_map.end()) continue;

                auto const& edges_to_expand = it->second;
                size_t const port_arity = edges_to_expand.size();
                if (port_arity <= 1) continue;

                g.nodes.emplace_back(Sum(port_arity));
                g.node_ids.push_back(generated_node_id(builder_id, g.node_ids.size()));
                size_t const sum_node = g.nodes.size() - 1;

                for (size_t out_port = 0; out_port < edges_to_expand.size(); ++out_port)
                {
                    GraphEdge const& to_rewire = edges_to_expand[out_port];
                    g.edges.erase(to_rewire);
                    g.edges.insert(GraphEdge{ to_rewire.source, { sum_node, out_port } });
                }
                g.edges.insert(GraphEdge{ { sum_node, 0 }, { node, in_port } });
            }
        }

        std::unordered_map<PortId, std::vector<GraphEdge>> edges_map;
        for (GraphEdge const& edge : g.edges)
        {
            edges_map[edge.source].push_back(edge);
        }

        nodes_size = g.nodes.size();
        for (size_t node = 0; node < nodes_size; ++node)
        {
            size_t const num_outputs = get_num_outputs(g.nodes[node]);
            for (size_t out_port = 0; out_port < num_outputs; ++out_port)
            {
                auto it = edges_map.find({ node, out_port });
                if (it == edges_map.end()) continue;

                auto const& edges_to_expand = it->second;
                size_t const port_arity = edges_to_expand.size();
                if (port_arity <= 1) continue;

                g.nodes.emplace_back(Broadcast(port_arity));
                g.node_ids.push_back(generated_node_id(builder_id, g.node_ids.size()));
                size_t const broadcast_node = g.nodes.size() - 1;

                for (size_t in_port = 0; in_port < edges_to_expand.size(); ++in_port)
                {
                    GraphEdge const& to_rewire = edges_to_expand[in_port];
                    g.edges.erase(to_rewire);
                    g.edges.insert(GraphEdge{ { broadcast_node, in_port }, to_rewire.target });
                }
                g.edges.insert(GraphEdge{ { node, out_port }, { broadcast_node, 0 } });
            }
        }
    }

    inline void stub_dangling_ports(PreparedGraph& g, size_t num_public_inputs, std::string_view builder_id)
    {
        auto [source_of, target_of] = make_source_target_edge_maps(g);

        size_t const num_nodes = g.nodes.size();
        for (size_t node_id = 0; node_id < num_nodes + 1; ++node_id)
        {
            size_t const node = (node_id == num_nodes) ? GRAPH_ID : node_id;
            size_t const num_outputs = (node == GRAPH_ID) ? num_public_inputs : get_num_outputs(g.nodes[node]);

            for (size_t output_port = 0; output_port < num_outputs; ++output_port)
            {
                PortId const this_port{ node, output_port };
                if (auto it = target_of.find(this_port); it == target_of.end())
                {
                    g.nodes.emplace_back(DummySink());
                    g.node_ids.push_back(generated_node_id(builder_id, g.node_ids.size()));
                    size_t const new_node = g.nodes.size() - 1;
                    g.edges.insert(GraphEdge{ this_port, { new_node, 0 } });
                }
            }
        }
    }

    inline void validate_graph(PreparedGraph const& g, size_t num_public_inputs, size_t num_public_outputs)
    {
        for (auto const& edge : g.edges)
        {
            size_t source_num_outputs = (edge.source.node == GRAPH_ID)
                ? num_public_inputs
                : get_num_outputs(g.nodes[edge.source.node]);

            size_t target_num_inputs = (edge.target.node == GRAPH_ID)
                ? num_public_outputs
                : get_num_inputs(g.nodes[edge.target.node]);

            if (edge.source.port >= source_num_outputs) {
                error("bad connection: source output port out of range");
            }
            if (edge.target.port >= target_num_inputs) {
                error("bad connection: target input port out of range");
            }
        }
    }

    inline auto make_node_adjacency(PreparedGraph const& g)
    {
        size_t const num_nodes = g.nodes.size();
        std::vector<std::unordered_set<size_t>> outgoing(num_nodes);
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

        return std::make_pair(std::move(outgoing), std::move(indegree));
    }

    inline void apply_node_permutation(PreparedGraph& g, std::vector<size_t> const& sorted)
    {
        size_t const num_nodes = g.nodes.size();

        std::vector<TypeErasedNode> sorted_nodes;
        std::vector<std::string> sorted_node_ids;
        sorted_nodes.reserve(num_nodes);
        sorted_node_ids.reserve(num_nodes);
        for (size_t old_i = 0; old_i < num_nodes; ++old_i)
        {
            sorted_nodes.push_back(std::move(g.nodes[sorted[old_i]]));
            sorted_node_ids.push_back(std::move(g.node_ids[sorted[old_i]]));
        }
        g.nodes.swap(sorted_nodes);
        g.node_ids.swap(sorted_node_ids);

        std::vector<size_t> reverse_sorted(num_nodes);
        for (size_t new_i = 0; new_i < num_nodes; ++new_i) {
            reverse_sorted[sorted[new_i]] = new_i;
        }

        std::unordered_set<GraphEdge> sorted_edges;
        sorted_edges.reserve(g.edges.size());
        for (GraphEdge edge : g.edges)
        {
            if (edge.source.node != GRAPH_ID)
                edge.source.node = reverse_sorted[edge.source.node];
            if (edge.target.node != GRAPH_ID)
                edge.target.node = reverse_sorted[edge.target.node];
            sorted_edges.insert(edge);
        }
        g.edges.swap(sorted_edges);

        for (auto& [_, info] : g.detached_info_by_source)
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

    inline void sort_nodes_or_error(PreparedGraph& g, std::string_view builder_id)
    {
        auto [outgoing, indegree] = make_node_adjacency(g);

        std::deque<size_t> ready;
        for (size_t node = 0; node < indegree.size(); ++node) {
            if (indegree[node] == 0) {
                ready.push_back(node);
            }
        }

        std::vector<size_t> sorted;
        sorted.reserve(g.nodes.size());

        while (!ready.empty())
        {
            size_t const node = ready.front();
            ready.pop_front();
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

    inline bool has_path(
        std::vector<std::unordered_set<size_t>> const& outgoing,
        size_t start,
        size_t goal)
    {
        if (start == goal) {
            return true;
        }

        std::vector<bool> seen(outgoing.size(), false);
        std::deque<size_t> q;
        q.push_back(start);
        seen[start] = true;

        while (!q.empty())
        {
            size_t u = q.front();
            q.pop_front();

            for (size_t v : outgoing[u])
            {
                if (seen[v]) continue;
                if (v == goal) return true;
                seen[v] = true;
                q.push_back(v);
            }
        }

        return false;
    }

    inline void validate_detached_edges(PreparedGraph const& g, std::string_view builder_id)
    {
        size_t const num_nodes = g.nodes.size();

        std::vector<std::unordered_set<size_t>> explicit_outgoing(num_nodes);
        std::unordered_map<PortId, std::vector<PortId>> consumers_of_output;

        for (GraphEdge const& edge : g.edges)
        {
            consumers_of_output[edge.source].push_back(edge.target);

            if (edge.source.node == GRAPH_ID) continue;
            if (edge.target.node == GRAPH_ID) continue;

            explicit_outgoing[edge.source.node].insert(edge.target.node);
        }

        for (auto const& [_, info] : g.detached_info_by_source)
        {
            if (info.original_source.node == GRAPH_ID) {
                continue;
            }

            auto it = consumers_of_output.find(info.reader_output);
            if (it == consumers_of_output.end()) {
                continue;
            }

            for (PortId target_port : it->second)
            {
                if (target_port.node == GRAPH_ID) {
                    continue;
                }

                size_t const u = info.original_source.node;
                size_t const v = target_port.node;

                auto augmented = explicit_outgoing;
                if (!has_path(augmented, v, u)) {
                    error(
                        "builder " + std::string(builder_id) + ": detach() on " +
                        "signal " + std::to_string(info.original_source.node) + ":" + std::to_string(info.original_source.port) +
                        " breaks an acyclic dependency"
                    );
                }
            }
        }
    }

    inline GraphExecutionPlan build_execution_plan(
        std::vector<TypeErasedNode> const& nodes,
        std::unordered_set<GraphEdge> const& edges,
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
        std::unordered_map<PortId, std::vector<PortId>> consumers;
        for (auto const& edge : edges) {
            consumers[edge.source].push_back(edge.target);
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
            for (PortId consumer : it->second) {
                if (consumer.node == GRAPH_ID) {
                    continue;
                }
                size_t const source_region = plan.node_to_region[detached_info.original_source.node];
                size_t const consumer_region = plan.node_to_region[consumer.node];
                if (source_region == consumer_region) {
                    plan.regions[source_region].max_block_size = std::min(
                        plan.regions[source_region].max_block_size,
                        detached_info.loop_block_size
                    );
                }
            }
        }

        for (GraphRegion& region : plan.regions) {
            std::unordered_set<size_t> region_nodes(region.nodes.begin(), region.nodes.end());
            std::unordered_map<size_t, std::unordered_set<size_t>> local_outgoing_sets;
            std::unordered_map<size_t, std::vector<size_t>> local_outgoing;
            std::unordered_map<size_t, size_t> local_indegree;

            for (size_t node : region.nodes) {
                local_outgoing_sets[node] = {};
                local_outgoing[node] = {};
                local_indegree[node] = 0;
            }

            for (auto const& edge : edges) {
                if (edge.source.node == GRAPH_ID || edge.target.node == GRAPH_ID) {
                    continue;
                }
                if (!region_nodes.contains(edge.source.node) || !region_nodes.contains(edge.target.node)) {
                    continue;
                }
                local_outgoing_sets[edge.source.node].insert(edge.target.node);
            }

            for (size_t node : region.nodes) {
                auto& targets = local_outgoing[node];
                auto const& unique_targets = local_outgoing_sets[node];
                targets.assign(unique_targets.begin(), unique_targets.end());
                std::sort(targets.begin(), targets.end());
                for (size_t target : targets) {
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

                for (size_t target : local_outgoing[node]) {
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

        std::vector<std::unordered_set<size_t>> region_outgoing(plan.regions.size());
        std::vector<size_t> indegree(plan.regions.size(), 0);
        for (auto const& edge : edges) {
            if (edge.source.node == GRAPH_ID || edge.target.node == GRAPH_ID) {
                continue;
            }
            size_t const u = plan.node_to_region[edge.source.node];
            size_t const v = plan.node_to_region[edge.target.node];
            if (u != v && region_outgoing[u].insert(v).second) {
                ++indegree[v];
            }
        }

        std::deque<size_t> ready;
        for (size_t region = 0; region < indegree.size(); ++region) {
            if (indegree[region] == 0) {
                ready.push_back(region);
            }
        }

        while (!ready.empty()) {
            size_t region = ready.front();
            ready.pop_front();
            plan.region_order.push_back(region);
            for (size_t target : region_outgoing[region]) {
                if (--indegree[target] == 0) {
                    ready.push_back(target);
                }
            }
        }

        return plan;
    }

    class LatencyAccumulator {
        std::unordered_map<PortId, size_t> _input_port_global_latencies;

    public:
        template<typename NodeLike, typename TargetMap>
        void align_latencies(
            NodeLike const& node,
            size_t node_i,
            std::span<InputConfig const> input_configs,
            std::span<OutputConfig const> output_configs,
            TargetMap const& target_of
        )
        {
            size_t node_global_latency = 0;
            for (size_t in_port = 0; in_port < input_configs.size(); ++in_port) {
                node_global_latency = std::max(node_global_latency, _input_port_global_latencies[{ node_i, in_port }]);
            }

            if (node_i == GRAPH_ID) {
                return;
            }

            node_global_latency += get_internal_latency(node);
            for (size_t out_port = 0; out_port < output_configs.size(); ++out_port) {
                size_t const output_latency = node_global_latency + output_configs[out_port].latency;
                if (auto it = target_of.find({ node_i, out_port }); it != target_of.end()) {
                    _input_port_global_latencies[it->second] = output_latency;
                }
            }
        }

        size_t delay_input(PortId input, size_t extra_delay)
        {
            return _input_port_global_latencies.at(input) += extra_delay;
        }
    };

    inline auto make_node_configs(
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

    inline size_t connection_block_size(
        PortId source,
        PortId target,
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

    inline GraphBuildArtifact build_graph_artifact(
        std::string graph_id,
        std::vector<TypeErasedNode> nodes,
        std::vector<std::string> node_ids,
        std::unordered_set<GraphEdge> edges,
        std::vector<DetachedInfo> detached,
        GraphExecutionPlan execution_plan,
        std::vector<InputConfig> public_inputs,
        std::vector<OutputConfig> public_outputs
    )
    {
        auto [source_of, target_of] = [&] {
            std::unordered_map<PortId, PortId> source_of_;
            std::unordered_map<PortId, PortId> target_of_;
            for (GraphEdge const& edge : edges) {
                source_of_[edge.target] = edge.source;
                target_of_[edge.source] = edge.target;
            }
            return std::make_tuple(std::move(source_of_), std::move(target_of_));
        }();

        std::vector<InputConfig> private_input_configs(public_outputs.size());
        OutputConfig private_outputs_config;
        LatencyAccumulator latency_accumulator;
        std::vector<std::vector<PortBufferPlan>> node_input_buffer_plans(nodes.size());
        std::vector<PortBufferPlan> public_output_buffer_plans(public_outputs.size());

        for (size_t node_i = 0; node_i < nodes.size() + 1; ++node_i) {
            if (node_i < nodes.size()) {
                auto [input_configs, output_configs] = make_node_configs(nodes[node_i], node_i, private_input_configs);
                latency_accumulator.align_latencies(nodes[node_i], node_i, input_configs, output_configs, target_of);

                for (size_t input_i = 0; input_i < input_configs.size(); ++input_i) {
                    PortId const this_input { node_i, input_i };

                    if (auto it = source_of.find(this_input); it != source_of.end()) {
                        size_t const output_node_i = it->second.node;
                        size_t const output_port_i = it->second.port;
                        OutputConfig const output_config = (output_node_i == GRAPH_ID)
                            ? private_outputs_config
                            : nodes[output_node_i].outputs()[output_port_i];
                        size_t const corrected_latency = latency_accumulator.delay_input(this_input, output_config.latency);
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
                std::vector<InputConfig> input_configs(public_outputs.size());

                for (size_t input_i = 0; input_i < input_configs.size(); ++input_i) {
                    PortId const this_input { GRAPH_ID, input_i };
                    if (auto it = source_of.find(this_input); it != source_of.end()) {
                        size_t const output_node_i = it->second.node;
                        size_t const output_port_i = it->second.port;
                        OutputConfig const output_config = (output_node_i == GRAPH_ID)
                            ? private_outputs_config
                            : nodes[output_node_i].outputs()[output_port_i];
                        size_t const corrected_latency = latency_accumulator.delay_input(this_input, output_config.latency);
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
            .detached = std::move(detached),
            .execution_plan = std::move(execution_plan),
            .public_inputs = std::move(public_inputs),
            .public_outputs = std::move(public_outputs),
            .public_output_buffer_plans = std::move(public_output_buffer_plans),
            .internal_latency = 0,
            .node_ids = std::move(node_ids),
        };
        {
            std::unordered_map<PortId, PortId> artifact_target_of;
            for (GraphEdge const& edge : artifact.edges) {
                artifact_target_of[edge.source] = edge.target;
            }

            std::unordered_map<PortId, size_t> input_global_latencies;
            size_t max_latency = 0;

            auto process_node = [&](TypeErasedNode const& node, size_t node_i) {
                size_t node_global_latency = 0;
                auto node_inputs = node.inputs();
                for (size_t input_port = 0; input_port < node_inputs.size(); ++input_port) {
                    node_global_latency = std::max(node_global_latency, input_global_latencies[{ node_i, input_port }]);
                }

                node_global_latency += node.internal_latency();
                auto node_outputs = node.outputs();
                for (size_t output_port = 0; output_port < node_outputs.size(); ++output_port) {
                    if (auto it = artifact_target_of.find({ node_i, output_port }); it != artifact_target_of.end()) {
                        size_t const new_latency = node_global_latency + node_outputs[output_port].latency;
                        max_latency = std::max(max_latency, new_latency);
                        input_global_latencies[it->second] = new_latency;
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

        artifact.scc_wrappers.reserve(artifact.execution_plan.region_order.size());
        for (size_t ordered_scc_i = 0; ordered_scc_i < artifact.execution_plan.region_order.size(); ++ordered_scc_i) {
            size_t const region_i = artifact.execution_plan.region_order[ordered_scc_i];
            auto const& region = artifact.execution_plan.regions[region_i];
            std::vector<GraphNodeWrapper> region_nodes;
            region_nodes.reserve(region.execution_order.size());

            for (size_t global_i : region.execution_order) {
                std::vector<std::string> output_targets;
                auto outputs = nodes[global_i].outputs();
                output_targets.reserve(outputs.size());
                for (size_t output_i = 0; output_i < outputs.size(); ++output_i) {
                    auto it = target_of.find({ global_i, output_i });
                    if (it != target_of.end()) {
                        if (it->second.node == GRAPH_ID) {
                            output_targets.push_back(
                                graph_port_data_export_id(
                                    artifact.graph_id,
                                    it->second.port
                                )
                            );
                        } else {
                            output_targets.push_back(
                                port_data_export_id(
                                    artifact.node_ids[it->second.node],
                                    it->second.port
                                )
                            );
                        }
                    } else {
                        output_targets.push_back({});
                    }
                }
                region_nodes.emplace_back(
                    std::move(nodes[global_i]),
                    std::move(node_input_buffer_plans[global_i]),
                    artifact.node_ids[global_i],
                    std::move(output_targets)
                );
            }

            size_t internal_latency = 0;
            // TODO: solve SCC-local internal latency explicitly instead of collapsing to the max child latency.
            for (auto const& node : region_nodes) {
                internal_latency = std::max(internal_latency, node.internal_latency());
            }

            artifact.scc_wrappers.emplace_back(
                std::move(region_nodes),
                region.max_block_size,
                internal_latency
            );
        }

        return artifact;
    }
}
