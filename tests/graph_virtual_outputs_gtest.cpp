#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/basic_nodes/routing.h>

#include <gtest/gtest.h>

namespace iv {

TEST(GraphVirtualOutputsTest, EnumeratesVirtualNodeOutputPorts)
{
    GraphBuilder g;
    auto node = _annotate_node_source_info(g.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(), "node-1");
    (void)node;

    auto const outputs = g.virtual_outputs();
    ASSERT_EQ(outputs.sample.size(), 1u);
    auto const &output = outputs.sample.front();
    EXPECT_EQ(output.virtual_node_id, "node-1");
    EXPECT_EQ(output.member_ordinal, 0u);
    EXPECT_EQ(output.source.port_ordinal, 0u);
    EXPECT_FALSE(output.has_existing_downstream_connection);
}

TEST(GraphVirtualOutputsTest, ReportsExistingDownstreamConnection)
{
    GraphBuilder g;
    auto node = _annotate_node_source_info(g.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(), "node-1");
    auto sink = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
    sink(node);

    auto const outputs = g.virtual_outputs();
    ASSERT_EQ(outputs.sample.size(), 1u);
    EXPECT_TRUE(outputs.sample.front().has_existing_downstream_connection);
}

TEST(GraphVirtualOutputsTest, ReportsAuthoredEventDownstreamConnection)
{
    GraphBuilder g;
    auto source = _annotate_node_source_info(
        g.node<EventConcatenation>(1, EventTypeId::empty).node_ref(),
        "event-source");
    auto sink = g.node<DummyEventSink>();
    sink.connect_event_input(0, source.event_port());

    auto const outputs = g.virtual_outputs();
    ASSERT_EQ(outputs.event.size(), 1u);
    EXPECT_EQ(outputs.event.front().virtual_node_id, "event-source");
    EXPECT_TRUE(outputs.event.front().has_existing_downstream_connection);
}

TEST(GraphVirtualOutputsTest, PublicEventInputConnectivityUsesAuthoredConnections)
{
    GraphBuilder g;
    auto input = g.event_input(EventTypeId::empty);
    auto sink = g.node<DummyEventSink>();
    sink.connect_event_input(0, input);

    EXPECT_TRUE(g.public_event_input_is_connected(0));
}

TEST(GraphVirtualOutputsTest, GroupsConcreteMembersOfSharedVirtualNode)
{
    GraphBuilder g;
    auto a = _annotate_node_source_info(g.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(), "shared");
    auto b = _annotate_node_source_info(g.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(), "shared");
    (void)a;
    (void)b;

    auto const outputs = g.virtual_outputs();
    ASSERT_EQ(outputs.sample.size(), 2u);
    EXPECT_EQ(outputs.sample[0].virtual_node_id, "shared");
    EXPECT_EQ(outputs.sample[1].virtual_node_id, "shared");
    EXPECT_NE(outputs.sample[0].member_ordinal, outputs.sample[1].member_ordinal);
}

TEST(GraphVirtualOutputsTest, GroupsStereoChannelOutputsIntoOneFamily)
{
    GraphBuilder g;
    auto node = _annotate_node_source_info(
        g.node<Sum<stereo, SampleStreamLayout::planar, 1>>().node_ref(), "stereo");
    (void)node;

    auto const families = g.virtual_sample_output_families();
    ASSERT_EQ(families.families.size(), 1u);
    auto const& family = families.families.front();
    EXPECT_EQ(family.virtual_node_id, "stereo");
    EXPECT_EQ(family.family_ordinal, 0u);
    EXPECT_EQ(family.channel_type, ChannelTypeId::stereo);
    EXPECT_EQ(family.channels.size(), 2u);
    ASSERT_EQ(family.channels[0].sources.size(), 1u);
    ASSERT_EQ(family.channels[1].sources.size(), 1u);
}

TEST(GraphVirtualOutputsTest, ReportsAuthoredStereoChannelConnectivity)
{
    GraphBuilder g;
    auto source = g.node<Sum<stereo, SampleStreamLayout::planar, 1>>();
    auto annotated = _annotate_node_source_info(source.node_ref(), "stereo");
    (void)annotated;
    auto sink = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
    sink(source.static_output<0>()[stereo::left]);

    auto const families = g.virtual_sample_output_families();
    ASSERT_EQ(families.families.size(), 1u);
    auto const& family = families.families.front();
    ASSERT_EQ(family.channels.size(), 2u);
    EXPECT_TRUE(family.channels[stereo::left.channel_ordinal]
                    .has_existing_downstream_connection);
    EXPECT_FALSE(family.channels[stereo::right.channel_ordinal]
                     .has_existing_downstream_connection);
}

TEST(GraphVirtualOutputsTest, MetadataReportsMixedAuthoredStereoConnectivity)
{
    GraphBuilder g;
    auto source = g.node<Sum<stereo, SampleStreamLayout::planar, 1>>();
    auto annotated = _annotate_node_source_info(source.node_ref(), "stereo");
    (void)annotated;
    auto sink = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
    sink(source.static_output<0>()[stereo::left]);

    auto const metadata = g.build_metadata();
    auto const it = std::find_if(
        metadata.virtual_nodes.begin(), metadata.virtual_nodes.end(),
        [](auto const& node) { return node.id == "stereo"; });
    ASSERT_NE(it, metadata.virtual_nodes.end());
    ASSERT_EQ(it->sample_outputs.size(), 1u);
    EXPECT_EQ(it->sample_outputs.front().connectivity,
              VirtualPortConnectivity::mixed);
}

TEST(GraphVirtualOutputsTest, NamedChannelOutputsKeepNameAndChannelAsSeparateIdentity)
{
    GraphBuilder g;
    g.outputs("main"_P[stereo::left] = 0.0f, "main"_P[stereo::right] = 0.0f);

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

TEST(GraphVirtualOutputsTest, RepeatedNamedPublicOutputsShareOneSummedGraphPort)
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

TEST(GraphVirtualOutputsTest, NamedChannelOutputsFormOneNamedStereoFamily)
{
    GraphBuilder g;
    g.outputs("main"_P[stereo::left] = 0.25f, "main"_P[stereo::right] = 0.25f);

    auto const built = g.build_root_node();
    auto const outputs = built.graph.outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs[0].name, "main");
    EXPECT_EQ(outputs[0].channel_layout.channel_type, ChannelTypeId::stereo);
}

TEST(GraphVirtualOutputsTest, WholeStreamAndChannelContributorsShareOneTypedPublicOutput)
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

TEST(GraphVirtualOutputsTest, NamedChannelOutputContributionsShareTheirStereoFamily)
{
    GraphBuilder g;
    g.outputs("main"_P[stereo::left] = 0.25f, "main"_P[stereo::right] = 0.5f);

    auto const built = g.build_root_node();
    auto const outputs = built.graph.outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front().name, "main");
}

TEST(GraphVirtualOutputsTest, NamedPublicPortsRejectMixedChannelTypes)
{
    GraphBuilder g;
    g.outputs("main"_P = 0.25f);
    g.outputs("main"_P[stereo::left] = 0.25f);

    EXPECT_THROW((void)g.public_sample_output_families(), std::logic_error);
}

TEST(GraphVirtualOutputsTest, ChannelQualifiedDescriptorsAreNotNodeArguments)
{
    using ChannelArgument = decltype("frequency"_P[mono::center] = 0.25f);
    static_assert(!details::valid_node_call_args_v<ChannelArgument>);
}

TEST(GraphVirtualOutputsTest, ChannelPortsSupportConstexprEquality)
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

TEST(GraphVirtualOutputsTest, FunctionalSubgraphUsesExplicitBoundaryFacade)
{
    GraphBuilder g;
    auto nested = g.subgraph([&](SubgraphBuilder& boundary) {
        auto input = boundary.input<"in">(0.0f);
        auto passthrough = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
        passthrough(input);
        boundary.outputs("out"_P = passthrough);
    });

    nested("in"_P = 0.25f);
    g.outputs("main"_P = nested);

    auto const built = g.build_root_node();
    ASSERT_EQ(built.graph.outputs().size(), 1u);
    EXPECT_EQ(built.graph.outputs().front().name, "main");
}

TEST(GraphVirtualOutputsTest, FunctionalSubgraphsCanNestWithoutAmbientScopeState)
{
    GraphBuilder g;
    auto outer = g.subgraph([&](SubgraphBuilder& outer_boundary) {
        auto outer_input = outer_boundary.input<"in">(0.0f);
        auto inner = g.subgraph([&](SubgraphBuilder& inner_boundary) {
            auto inner_input = inner_boundary.input<"in">(0.0f);
            auto passthrough = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
            passthrough(inner_input);
            inner_boundary.outputs("out"_P = passthrough);
        });
        inner("in"_P = outer_input);
        outer_boundary.outputs("out"_P = inner);
    });

    outer("in"_P = 0.5f);
    g.outputs("main"_P = outer);

    EXPECT_NO_THROW((void)g.build_root_node());
}

} // namespace iv
