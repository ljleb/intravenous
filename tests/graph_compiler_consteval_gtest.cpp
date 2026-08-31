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

    auto const plan = details::build_execution_plan(
        nodes, edges, std::flat_set<GraphEventEdge>{}, {});
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

static_assert(execution_plan_keeps_deterministic_topological_order());

TEST(GraphCompilerConsteval, ExecutionPlanKeepsDeterministicTopologicalOrder)
{
    EXPECT_TRUE(execution_plan_keeps_deterministic_topological_order());
}

} // namespace
} // namespace iv
