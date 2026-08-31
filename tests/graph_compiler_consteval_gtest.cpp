#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>

#include <gtest/gtest.h>

namespace iv {
namespace {

consteval bool execution_plan_keeps_deterministic_topological_order()
{
    auto const node = details::reflect_node(Constant{0.0f});
    std::vector<ReflectedNodeDescription> nodes{node, node, node, node};
    std::flat_set<GraphEdge> edges;
    edges.emplace(ConcretePortId{0, 0}, ConcretePortId{2, 0});
    edges.emplace(ConcretePortId{1, 0}, ConcretePortId{2, 0});
    edges.emplace(ConcretePortId{2, 0}, ConcretePortId{3, 0});

    ExecutableGraphData graph;
    graph.nodes = nodes;
    graph.edges = edges;
    auto const connectivity = details::build_compiler_connectivity(graph);

    auto const plan = details::build_execution_plan(
        nodes, connectivity, {});
    if (plan.regions.size() != 4 || plan.region_order.size() != 4) {
        return false;
    }

    std::vector<size_t> execution_order;
    for (size_t region : plan.region_order) {
        auto const& region_order = plan.regions[region].execution_order;
        execution_order.insert(
            execution_order.end(), region_order.begin(), region_order.end());
    }
    return execution_order == std::vector<size_t>{0, 1, 2, 3};
}

consteval bool reflected_nodes_share_type_operations_but_not_static_data()
{
    auto const low_gain = details::reflect_node(Constant{0.125f});
    auto const high_gain = details::reflect_node(Constant{0.875f});
    auto const& low = low_gain.operations.runtime;
    auto const& high = high_gain.operations.runtime;

    return low.node_data != high.node_data
        && low.declare_node == high.declare_node
        && low.tick_block == high.tick_block
        && low.skip_block == high.skip_block;
}

consteval bool node_permutation_remaps_all_structural_references()
{
    auto const node = details::reflect_node(Constant{0.0f});
    ExecutableGraphData graph;
    graph.nodes = {node, node, node};
    graph.explicit_ttl_samples = {std::nullopt, std::nullopt, std::nullopt};
    graph.node_ids = {"zero", "one", "two"};
    graph.node_virtual_ids.resize(3);
    graph.node_source_infos.resize(3);
    graph.node_construction_order = {0, 1, 2};
    graph.edges.emplace(ConcretePortId{0, 0}, ConcretePortId{1, 0});
    graph.detached_info_by_source.emplace(
        ConcretePortId{0, 0},
        DetachedInfo{
            .detach_id = 7,
            .original_source = {0, 0},
            .writer_node = 0,
            .reader_output = {2, 0},
        });
    graph.detached_reader_outputs.emplace(ConcretePortId{2, 0});

    LoweredSubgraphSpec scope;
    scope.member_nodes = {0, 2};
    scope.sample_input_targets = {{{.port = {0, 0}, .valid = true}}};
    scope.sample_output_sources = {
        {.port = {2, 0}, .valid = true},
        {.port = {0, 0}, .valid = false},
    };
    std::vector<LoweredSubgraphSpec> scopes{scope};

    details::apply_node_permutation(graph, {2, 0, 1}, scopes);

    auto const detached = graph.detached_info_by_source.find({1, 0});
    return graph.node_ids == std::vector<std::string>{"two", "zero", "one"}
        && graph.edges.contains(GraphEdge{{1, 0}, {2, 0}})
        && detached != graph.detached_info_by_source.end()
        && detached->second.original_source == ConcretePortId{1, 0}
        && detached->second.writer_node == 1
        && detached->second.reader_output == ConcretePortId{0, 0}
        && graph.detached_reader_outputs.contains({0, 0})
        && scopes[0].member_nodes == std::vector<size_t>{1, 0}
        && scopes[0].sample_input_targets[0][0].port == ConcretePortId{1, 0}
        && scopes[0].sample_output_sources[0].port == ConcretePortId{0, 0}
        && scopes[0].sample_output_sources[1].port == ConcretePortId{0, 0};
}

static_assert(execution_plan_keeps_deterministic_topological_order());
static_assert(node_permutation_remaps_all_structural_references());
static_assert(reflected_nodes_share_type_operations_but_not_static_data());

TEST(GraphCompilerConsteval, ExecutionPlanKeepsDeterministicTopologicalOrder)
{
    EXPECT_TRUE(execution_plan_keeps_deterministic_topological_order());
}

TEST(GraphCompilerConsteval, NodePermutationRemapsAllStructuralReferences)
{
    EXPECT_TRUE(node_permutation_remaps_all_structural_references());
}

TEST(GraphCompilerConsteval, ReflectedNodesShareTypeOperationsButNotStaticData)
{
    EXPECT_TRUE(reflected_nodes_share_type_operations_but_not_static_data());
}

} // namespace
} // namespace iv
