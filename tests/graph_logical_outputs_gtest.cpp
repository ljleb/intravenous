#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>

#include <gtest/gtest.h>

namespace iv {

namespace {
struct StereoOutputNode {
    static constexpr auto inputs()
    {
        return std::array<InputConfig, 1>{{
            {.name = "in", .default_value = 0.0f}
        }};
    }

    static constexpr auto outputs()
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
    static constexpr auto inputs()
    {
        return std::array<InputConfig, 1>{{
            {.name = "in", .default_value = 0.0f}
        }};
    }

    static constexpr auto outputs()
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
    auto node = _annotate_node_source_info(g.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(), "node-1");
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
    auto node = _annotate_node_source_info(g.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(), "node-1");
    auto sink = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
    sink(node);

    auto const outputs = g.logical_outputs();
    ASSERT_EQ(outputs.sample.size(), 1u);
    EXPECT_TRUE(outputs.sample.front().has_existing_downstream_connection);
}

TEST(GraphLogicalOutputsTest, GroupsConcreteMembersOfSharedLogicalNode)
{
    GraphBuilder g;
    auto a = _annotate_node_source_info(g.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(), "shared");
    auto b = _annotate_node_source_info(g.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(), "shared");
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

TEST(GraphLogicalOutputsTest, NamedChannelOutputsKeepNameAndChannelAsSeparateIdentity)
{
    GraphBuilder g;
    g.multi_channel<stereo>([&]<auto channel>() {
        g.outputs("main"_P[channel] = 0.0f);
    });

    auto const built = g.build_root_node();
    auto const outputs = built.graph.outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs[0].name, "main");
    EXPECT_EQ(outputs[0].channel_layout.channel_type, ChannelTypeId::stereo);
    auto const families = g.public_sample_output_families();
    ASSERT_EQ(families.families.size(), 1u);
    ASSERT_EQ(families.families.front().channels.size(), 2u);
    EXPECT_EQ(families.families.front().channels[0].port_ordinals, (std::vector<size_t>{0u}));
    EXPECT_EQ(families.families.front().channels[1].port_ordinals, (std::vector<size_t>{0u}));
}

TEST(GraphLogicalOutputsTest, RepeatedNamedPublicOutputsShareOneSummedGraphPort)
{
    GraphBuilder g;
    g.outputs("main"_P = 0.25f);
    g.outputs("main"_P = 0.5f);

    auto const built = g.build_root_node();
    auto const outputs = built.graph.outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front().name, "main");
    EXPECT_EQ(outputs.front().channel_layout.channel_type, ChannelTypeId::mono);
    auto const families = g.public_sample_output_families();
    ASSERT_EQ(families.families.size(), 1u);
    EXPECT_EQ(families.families.front().channels[0].port_ordinals, (std::vector<size_t>{0u}));
    // Both declarations target this one graph port. The graph's standard
    // multiple-source edge lowering is therefore responsible for summing them.
}

TEST(GraphLogicalOutputsTest, NamedChannelOutputsFormOneNamedStereoFamily)
{
    GraphBuilder g;
    g.multi_channel<stereo>([&]<auto c> {
        g.outputs("main"_P[c] = 0.25f);
    });

    auto const built = g.build_root_node();
    auto const outputs = built.graph.outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs[0].name, "main");
    EXPECT_EQ(outputs[0].channel_layout.channel_type, ChannelTypeId::stereo);
}

TEST(GraphLogicalOutputsTest, WholeStreamAndChannelContributorsShareOneTypedPublicOutput)
{
    GraphBuilder g;
    auto stereo_source = g.node<Sum<stereo, SampleStreamLayout::interleaved, 1>>();
    g.outputs("main"_P = stereo_source);
    g.outputs("main"_P[stereo::left] = 0.25f);

    auto const built = g.build_root_node();
    auto const outputs = built.graph.outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front().name, "main");
    EXPECT_EQ(outputs.front().channel_layout.channel_type, ChannelTypeId::stereo);
    EXPECT_EQ(outputs.front().channel_layout.sample_layout, SampleStreamLayout::planar);
    auto const families = g.public_sample_output_families();
    ASSERT_EQ(families.families.size(), 1u);
    EXPECT_EQ(families.families.front().channels[0].port_ordinals, (std::vector<size_t>{0u}));
    EXPECT_EQ(families.families.front().channels[1].port_ordinals, (std::vector<size_t>{0u}));
}

TEST(GraphLogicalOutputsTest, NamedChannelOutputContributionsShareTheirStereoFamily)
{
    GraphBuilder g;
    g.multi_channel<stereo>([&]<auto c> {
        g.outputs("main"_P[c] = 0.25f, "main"_P[swap_side(c)] = 0.5f);
    });

    auto const built = g.build_root_node();
    auto const outputs = built.graph.outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front().name, "main");
}

TEST(GraphLogicalOutputsTest, NamedPublicPortsRejectMixedChannelTypes)
{
    GraphBuilder g;
    g.outputs("main"_P = 0.25f);
    g.outputs("main"_P[stereo::left] = 0.25f);

    EXPECT_THROW((void)g.public_sample_output_families(), std::logic_error);
}

TEST(GraphLogicalOutputsTest, ChannelQualifiedDescriptorsAreNotNodeArguments)
{
    using ChannelArgument = decltype("frequency"_P[mono::center] = 0.25f);
    static_assert(!details::valid_node_call_args_v<ChannelArgument>);
}

TEST(GraphLogicalOutputsTest, ChannelPortsSupportConstexprEquality)
{
    constexpr auto is_left = []<auto channel>() {
        if constexpr (channel == stereo::left) {
            return true;
        }
        return false;
    };

    static_assert(is_left.template operator()<stereo::left>());
    static_assert(!is_left.template operator()<stereo::right>());
    static_assert(stereo::left != stereo::right);
    static_assert(mono::center != stereo::left);

    EXPECT_TRUE(is_left.template operator()<stereo::left>());
    EXPECT_FALSE(is_left.template operator()<stereo::right>());
}

TEST(GraphLogicalOutputsTest, MultiChannelReturnsIndexableChannelSampleRefs)
{
    GraphBuilder g;
    auto refs = g.multi_channel<stereo>([&]<auto channel>() {
        return channel = g.node<Constant>(0.25f);
    });
    static_assert(std::same_as<
        decltype(refs),
        ChannelRefs<stereo>>);

    g.multi_channel<stereo>([&]<auto channel>() {
        g.outputs("main"_P[channel] = refs[channel]);
    });

    auto const built = g.build_root_node();
    auto const outputs = built.graph.outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs[0].name, "main");
    auto const families = g.public_sample_output_families();
    ASSERT_EQ(families.families.size(), 1u);
    ASSERT_EQ(families.families.front().channels.size(), 2u);
    EXPECT_EQ(families.families.front().channels[0].port_ordinals, (std::vector<size_t>{0u}));
    EXPECT_EQ(families.families.front().channels[1].port_ordinals, (std::vector<size_t>{0u}));
}

} // namespace iv
