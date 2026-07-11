#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>

#include <gtest/gtest.h>

namespace iv {

namespace {
struct StereoOutputNode {
    auto inputs() const
    {
        return std::array<InputConfig, 1>{{
            {.name = "in", .default_value = 0.0f}
        }};
    }

    auto outputs() const
    {
        return std::array<OutputConfig, 2>{{
            {.name = "__stereo_left_0"},
            {.name = "__stereo_right_0"},
        }};
    }

    void tick(auto const& ctx) const
    {
        ctx.outputs[0].push(Sample{0.0f});
        ctx.outputs[1].push(Sample{0.0f});
    }
};

struct StereoLeftOnlyNode {
    auto inputs() const
    {
        return std::array<InputConfig, 1>{{
            {.name = "in", .default_value = 0.0f}
        }};
    }

    auto outputs() const
    {
        return std::array<OutputConfig, 1>{{
            {.name = "__stereo_left_0"},
        }};
    }

    void tick(auto const& ctx) const
    {
        ctx.outputs[0].push(Sample{0.0f});
    }
};
} // namespace

TEST(GraphLogicalOutputsTest, EnumeratesLogicalNodeOutputPorts)
{
    GraphBuilder g;
    auto node = _annotate_node_source_info(g.node<Sum<1>>().node_ref(), "node-1");
    (void)node;

    auto const outputs = g.logical_outputs();
    ASSERT_EQ(outputs.sample.size(), 1u);
    auto const &output = outputs.sample.front();
    EXPECT_EQ(output.logical_node_id, "node-1");
    EXPECT_EQ(output.member_ordinal, 0u);
    EXPECT_EQ(output.source.port, 0u);
    EXPECT_FALSE(output.has_existing_downstream_connection);
}

TEST(GraphLogicalOutputsTest, ReportsExistingDownstreamConnection)
{
    GraphBuilder g;
    auto node = _annotate_node_source_info(g.node<Sum<1>>().node_ref(), "node-1");
    auto sink = g.node<Sum<1>>();
    sink(node);

    auto const outputs = g.logical_outputs();
    ASSERT_EQ(outputs.sample.size(), 1u);
    EXPECT_TRUE(outputs.sample.front().has_existing_downstream_connection);
}

TEST(GraphLogicalOutputsTest, GroupsConcreteMembersOfSharedLogicalNode)
{
    GraphBuilder g;
    auto a = _annotate_node_source_info(g.node<Sum<1>>().node_ref(), "shared");
    auto b = _annotate_node_source_info(g.node<Sum<1>>().node_ref(), "shared");
    (void)a;
    (void)b;

    auto const outputs = g.logical_outputs();
    ASSERT_EQ(outputs.sample.size(), 2u);
    EXPECT_EQ(outputs.sample[0].logical_node_id, "shared");
    EXPECT_EQ(outputs.sample[1].logical_node_id, "shared");
    EXPECT_NE(outputs.sample[0].member_ordinal, outputs.sample[1].member_ordinal);
}

TEST(GraphLogicalOutputsTest, GroupsStereoChannelOutputsIntoOneFamily)
{
    GraphBuilder g;
    auto node = _annotate_node_source_info(g.node<StereoOutputNode>().node_ref(), "stereo");
    (void)node;

    auto const families = g.logical_sample_output_families();
    ASSERT_EQ(families.families.size(), 1u);
    auto const& family = families.families.front();
    EXPECT_EQ(family.logical_node_id, "stereo");
    EXPECT_EQ(family.family_ordinal, 0u);
    EXPECT_EQ(family.channel_type, ChannelTypeId::stereo);
    EXPECT_EQ(family.channels.size(), 2u);
    ASSERT_TRUE(family.channels[0].source.has_value());
    ASSERT_TRUE(family.channels[1].source.has_value());
}

TEST(GraphLogicalOutputsTest, KeepsSparseStereoFamiliesSparse)
{
    GraphBuilder g;
    auto node = _annotate_node_source_info(g.node<StereoLeftOnlyNode>().node_ref(), "stereo");
    (void)node;

    auto const families = g.logical_sample_output_families();
    ASSERT_EQ(families.families.size(), 1u);
    auto const& family = families.families.front();
    EXPECT_EQ(family.logical_node_id, "stereo");
    EXPECT_EQ(family.channel_type, ChannelTypeId::stereo);
    EXPECT_EQ(family.channels.size(), 2u);
    ASSERT_TRUE(family.channels[0].source.has_value());
    EXPECT_FALSE(family.channels[1].source.has_value());
}

TEST(GraphLogicalOutputsTest, AppendsPublicOutputsAcrossMultiChannelCalls)
{
    GraphBuilder g;
    g.multi_channel<ChannelTypeId::stereo>([&]<auto channel>() {
        g.outputs(channel = 0.0f);
    });

    auto const built = g.build_root_node();
    auto const outputs = built.graph.outputs();
    ASSERT_EQ(outputs.size(), 2u);
    EXPECT_EQ(outputs[0].name, "__stereo_left_0");
    EXPECT_EQ(outputs[1].name, "__stereo_right_0");
}

TEST(GraphLogicalOutputsTest, ChannelPortsSupportConstexprEquality)
{
    constexpr auto is_left = []<auto channel>() {
        if constexpr (channel == channels::stereo_left) {
            return true;
        }
        return false;
    };

    static_assert(is_left.template operator()<channels::stereo_left>());
    static_assert(!is_left.template operator()<channels::stereo_right>());
    static_assert(channels::stereo_left != channels::stereo_right);
    static_assert(channels::mono != channels::stereo_left);

    EXPECT_TRUE(is_left.template operator()<channels::stereo_left>());
    EXPECT_FALSE(is_left.template operator()<channels::stereo_right>());
}

TEST(GraphLogicalOutputsTest, MultiChannelReturnsIndexableChannelSampleRefs)
{
    GraphBuilder g;
    auto refs = g.multi_channel<ChannelTypeId::stereo>([&]<auto channel>() {
        return channel = g.node<Constant>(0.25f);
    });
    static_assert(std::same_as<
        decltype(refs),
        ChannelRefs<ChannelTypeId::stereo>>);

    g.multi_channel<ChannelTypeId::stereo>([&]<auto channel>() {
        g.outputs(channel = refs[channel]);
    });

    auto const outputs = g.build_root_node().graph.outputs();
    ASSERT_EQ(outputs.size(), 2u);
    EXPECT_EQ(outputs[0].name, "__stereo_left_0");
    EXPECT_EQ(outputs[1].name, "__stereo_right_0");
}

} // namespace iv
