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

consteval bool node_adjacency_deduplicates_sample_and_event_edges()
{
    auto const node = details::reflect_node(Constant{0.0f});
    ExecutableGraphData graph;
    graph.nodes = {node, node, node};
    graph.edges.emplace(ConcretePortId{0, 0}, ConcretePortId{2, 0});
    graph.edges.emplace(ConcretePortId{0, 1}, ConcretePortId{2, 1});
    graph.event_edges.emplace(ConcretePortId{0, 0}, ConcretePortId{2, 0});
    graph.edges.emplace(ConcretePortId{GRAPH_ID, 0}, ConcretePortId{1, 0});
    graph.event_edges.emplace(ConcretePortId{1, 0}, ConcretePortId{GRAPH_ID, 0});

    auto const [outgoing, indegree] = details::make_node_adjacency(graph);
    return outgoing == std::vector<std::vector<size_t>>{{2}, {}, {}}
        && indegree == std::vector<size_t>{0, 0, 1};
}

consteval bool dormancy_groups_are_ordered_by_a_single_hierarchy_walk()
{
    GraphBuildArtifact artifact{};
    auto make_group = [](size_t const parent_group, size_t const member_node) {
        DormancyGroup group{};
        group.parent_group = parent_group;
        group.member_nodes.push_back(member_node);
        return group;
    };
    artifact.dormancy_groups = {
        make_group(1, 0),
        make_group(GRAPH_ID, 1),
        make_group(0, 2),
        make_group(GRAPH_ID, 3),
        make_group(1, 4),
    };

    auto const graph = Graph(std::move(artifact));
    auto const groups = graph._dormancy_groups;
    return groups.size == 5
        && groups[0].parent_group == GRAPH_ID
        && groups[0].subtree_end_exclusive == 4
        && groups[0].member_nodes[0] == 1
        && groups[1].parent_group == 0
        && groups[1].subtree_end_exclusive == 3
        && groups[1].member_nodes[0] == 0
        && groups[2].parent_group == 1
        && groups[2].subtree_end_exclusive == 3
        && groups[2].member_nodes[0] == 2
        && groups[3].parent_group == 0
        && groups[3].subtree_end_exclusive == 4
        && groups[3].member_nodes[0] == 4
        && groups[4].parent_group == GRAPH_ID
        && groups[4].subtree_end_exclusive == 5
        && groups[4].member_nodes[0] == 3;
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
static_assert(node_adjacency_deduplicates_sample_and_event_edges());
static_assert(dormancy_groups_are_ordered_by_a_single_hierarchy_walk());
static_assert(node_permutation_remaps_all_structural_references());
static_assert(reflected_nodes_share_type_operations_but_not_static_data());

TEST(GraphCompilerConsteval, ExecutionPlanKeepsDeterministicTopologicalOrder)
{
    EXPECT_TRUE(execution_plan_keeps_deterministic_topological_order());
}

TEST(GraphCompilerConsteval, NodeAdjacencyDeduplicatesSampleAndEventEdges)
{
    EXPECT_TRUE(node_adjacency_deduplicates_sample_and_event_edges());
}

TEST(GraphCompilerConsteval, DormancyGroupsAreOrderedByASingleHierarchyWalk)
{
    EXPECT_TRUE(dormancy_groups_are_ordered_by_a_single_hierarchy_walk());
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
