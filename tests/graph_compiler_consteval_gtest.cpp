#include <intravenous/basic_nodes/type_erased.h>
#include <intravenous/graph/compiler.h>

#include <gtest/gtest.h>

namespace iv {
namespace {

constexpr bool same(InputConfig const& lhs, InputConfig const& rhs)
{
    return lhs.name == rhs.name
        && lhs.channel_layout == rhs.channel_layout
        && lhs.history == rhs.history
        && lhs.default_value == rhs.default_value
        && lhs.min == rhs.min
        && lhs.max == rhs.max;
}

constexpr bool same(OutputConfig const& lhs, OutputConfig const& rhs)
{
    return lhs.name == rhs.name
        && lhs.channel_layout == rhs.channel_layout
        && lhs.latency == rhs.latency
        && lhs.history == rhs.history;
}

constexpr bool same(
    ReflectedNodeDescription const& lhs,
    ReflectedNodeDescription const& rhs)
{
    if (lhs.inputs().size() != rhs.inputs().size()
        || lhs.outputs().size() != rhs.outputs().size()
        || lhs.event_inputs().size() != rhs.event_inputs().size()
        || lhs.event_outputs().size() != rhs.event_outputs().size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.inputs().size(); ++i) {
        if (!same(lhs.inputs()[i], rhs.inputs()[i])) return false;
    }
    for (size_t i = 0; i < lhs.outputs().size(); ++i) {
        if (!same(lhs.outputs()[i], rhs.outputs()[i])) return false;
    }
    return lhs.operations.runtime.declare_node
            == rhs.operations.runtime.declare_node
        && lhs.operations.runtime.tick_block
            == rhs.operations.runtime.tick_block
        && lhs.operations.runtime.skip_block
            == rhs.operations.runtime.skip_block
        && lhs.operations.apply_detach_id_offset
            == rhs.operations.apply_detach_id_offset
        && lhs.type_name == rhs.type_name
        && lhs.internal_latency_samples == rhs.internal_latency_samples
        && lhs.maximum_block_size == rhs.maximum_block_size
        && lhs.default_ttl_samples == rhs.default_ttl_samples
        && lhs.block_skippable == rhs.block_skippable;
}

template<size_t Arity, class ChannelType, SampleStreamLayout Layout>
consteval bool lazy_broadcast_reflection_matches_direct_reflection()
{
    auto const lazy = details::make_broadcast_node(
        Arity,
        ChannelLayout{
            .channel_type = ChannelTypeTraits<ChannelType>::id,
            .sample_layout = Layout,
        });
    auto const direct = details::reflect_node(
        Broadcast<Arity, ChannelType, Layout>{});
    return same(lazy, direct);
}

static_assert(lazy_broadcast_reflection_matches_direct_reflection<
    1, mono, SampleStreamLayout::planar>());
static_assert(lazy_broadcast_reflection_matches_direct_reflection<
    2, mono, SampleStreamLayout::interleaved>());
static_assert(lazy_broadcast_reflection_matches_direct_reflection<
    17, stereo, SampleStreamLayout::planar>());
static_assert(lazy_broadcast_reflection_matches_direct_reflection<
    64, stereo, SampleStreamLayout::interleaved>());

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

TEST(GraphCompilerConsteval, LazyBroadcastKeepsExactValueSpecializedOperations)
{
    EXPECT_TRUE((lazy_broadcast_reflection_matches_direct_reflection<
        1, mono, SampleStreamLayout::planar>()));
    EXPECT_TRUE((lazy_broadcast_reflection_matches_direct_reflection<
        64, stereo, SampleStreamLayout::interleaved>()));
}

TEST(GraphCompilerConsteval, ExecutionPlanKeepsDeterministicTopologicalOrder)
{
    EXPECT_TRUE(execution_plan_keeps_deterministic_topological_order());
}

} // namespace
} // namespace iv
