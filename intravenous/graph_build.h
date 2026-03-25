#pragma once

#include "basic_nodes/routing.h"
#include "basic_nodes/arithmetic.h"
#include "graph_node.h"

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
        Graph::Nodes nodes;
        Graph::Edges edges;
        std::unordered_map<PortId, DetachedInfo> detached_info_by_source;
        std::unordered_set<PortId> detached_reader_outputs;
    };

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

    inline void expand_hyperedge_ports(PreparedGraph& g)
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

    inline void stub_dangling_ports(PreparedGraph& g, size_t num_public_inputs)
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

        Graph::Nodes sorted_nodes;
        sorted_nodes.reserve(num_nodes);
        for (size_t old_i = 0; old_i < num_nodes; ++old_i)
        {
            sorted_nodes.push_back(std::move(g.nodes[sorted[old_i]]));
        }
        g.nodes.swap(sorted_nodes);

        std::vector<size_t> reverse_sorted(num_nodes);
        for (size_t new_i = 0; new_i < num_nodes; ++new_i) {
            reverse_sorted[sorted[new_i]] = new_i;
        }

        Graph::Edges sorted_edges;
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
        Graph::Edges const& edges,
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
}
