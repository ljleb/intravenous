#include <intravenous/graph/builder.h>
#include <intravenous/graph/reflected_node.hpp>

#include <intravenous/graph/builder/lowering.cpp>
#include <intravenous/graph/builder/metadata.cpp>
#include <intravenous/graph/builder/finalize.cpp>

#include <array>

struct FinalizeGainNode {
    int gain = 1;

    constexpr auto inputs() const
    {
        return std::array<iv::InputConfig, 1>{};
    }

    constexpr auto outputs() const
    {
        return std::array<iv::OutputConfig, 1>{};
    }

    void tick(iv::TickSampleContext<FinalizeGainNode> const& ctx) const
    {
        ctx.outputs[0].push(ctx.inputs[0].get() * gain);
    }
};

namespace iv::details {
struct GraphBuilderTestAccess {
    static consteval bool finalize_graph()
    {
        GraphBuilder graph;
        auto source = graph.node<Constant>(Sample{2});
        auto gain = graph.node<FinalizeGainNode>(3);
        gain(source);
        graph.outputs(gain);
        auto lowered = GraphBuilderLowering::lower(
            graph._node_bundles,
            graph._connections,
            graph._public_ports,
            graph._virtual_nodes,
            graph._detach,
            true);
        PreparedBuilderGraph prepared(
            graph._identity,
            lowered,
            graph._node_bundles,
            graph._virtual_nodes);
        prepared.append_reflected_nodes(0);
        prepared.lower_edges();
        prepared.add_subgraph_default_edges();
        prepared.copy_detach_info();
        expand_hyperedge_ports(prepared.graph, graph._identity.value);
        stub_dangling_ports(prepared.graph, 0, graph._identity.value);
        validate_graph(prepared.graph, 0, 1);
        validate_detached_edges(prepared.graph, graph._identity.value);
        sort_nodes_or_error(prepared.graph, graph._identity.value);
        validate_graph(prepared.graph, 0, 1);
        auto scopes = prepared.build_lowered_scopes();
        auto lowered_subgraphs = compile_lowered_subgraphs(
            prepared.graph, scopes);
        auto [virtual_nodes, virtual_by_backing] = build_virtual_metadata(
            prepared.graph, scopes);
        std::vector<iv::DetachedInfo> detached;
        for (auto const& [_, info] : prepared.graph.detached_info_by_source) {
            detached.push_back(info);
        }
        auto execution_plan = build_execution_plan(
            prepared.graph.nodes,
            prepared.graph.edges,
            prepared.graph.event_edges,
            detached);
        auto dormancy_groups = compile_dormancy_groups(
            prepared.graph,
            lowered_subgraphs,
            graph._identity.value,
            execution_plan);
        auto artifact = build_graph_artifact(
            graph._identity.value,
            std::move(prepared.graph.nodes),
            std::move(prepared.graph.explicit_ttl_samples),
            std::move(prepared.graph.node_ids),
            std::move(prepared.graph.edges),
            std::move(prepared.graph.event_edges),
            std::move(detached),
            std::move(execution_plan),
            {},
            {iv::OutputConfig{}},
            {},
            {},
            std::move(dormancy_groups));
        (void)std::meta::reflect_constant(
            artifact.scc_wrappers.front()._nodes.front());
        (void)std::meta::reflect_constant(artifact.scc_wrappers.front());
        return !artifact.scc_wrappers.empty() && !artifact.edges.empty();
    }
};
}

static_assert(iv::details::GraphBuilderTestAccess::finalize_graph());

int main() {}
