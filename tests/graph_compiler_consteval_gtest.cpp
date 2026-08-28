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
        && lhs.operations.apply_detach_id_offset_impl
            == rhs.operations.apply_detach_id_offset_impl
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

consteval bool detach_offset_keeps_exact_value_specialization()
{
    auto const writer = details::reflect_node(DetachWriterNode{
        .id = DetachArrayId{7},
        .loop_extra_latency = 3,
    });
    auto const adjusted = writer.operations.apply_detach_id_offset(11);
    auto const expected = details::reflect_node(DetachWriterNode{
        .id = DetachArrayId{18},
        .loop_extra_latency = 3,
    });
    return same(
        ReflectedNodeDescription{.operations = adjusted},
        ReflectedNodeDescription{.operations = expected.operations});
}

consteval bool ordinary_nodes_skip_detach_rewrite_callback()
{
    auto const node = details::reflect_node(Broadcast<2>{});
    return node.operations.apply_detach_id_offset_impl == nullptr
        && node.operations.apply_detach_id_offset(42).runtime.tick_block
            == node.operations.runtime.tick_block;
}

static_assert(detach_offset_keeps_exact_value_specialization());
static_assert(ordinary_nodes_skip_detach_rewrite_callback());

TEST(GraphCompilerConsteval, LazyBroadcastKeepsExactValueSpecializedOperations)
{
    EXPECT_TRUE((lazy_broadcast_reflection_matches_direct_reflection<
        1, mono, SampleStreamLayout::planar>()));
    EXPECT_TRUE((lazy_broadcast_reflection_matches_direct_reflection<
        64, stereo, SampleStreamLayout::interleaved>()));
}

TEST(GraphCompilerConsteval, DetachRewritesOnlyDetachOperations)
{
    EXPECT_TRUE(detach_offset_keeps_exact_value_specialization());
    EXPECT_TRUE(ordinary_nodes_skip_detach_rewrite_callback());
}

} // namespace
} // namespace iv
