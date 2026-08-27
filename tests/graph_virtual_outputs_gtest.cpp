#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/basic_nodes/routing.h>

#include <gtest/gtest.h>

namespace iv {
namespace {

struct VirtualOutputSnapshot {
    size_t sample_count = 0;
    size_t event_count = 0;
    bool id_prefix = false;
    size_t first_member = 0;
    size_t first_port = 0;
    bool first_connected = false;
    bool second_connected = false;
    bool shared_id = false;
    bool distinct_members = false;
};

consteval VirtualOutputSnapshot single_sample_output_snapshot(bool connected)
{
    GraphBuilder g;
    auto node = _annotate_node_source_info(
        g.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(),
        "node-1");
    if (connected) {
        auto sink = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
        sink(node);
    }
    auto const outputs = g.virtual_outputs();
    return {
        .sample_count = outputs.sample.size(),
        .event_count = outputs.event.size(),
        .id_prefix = !outputs.sample.empty()
            && outputs.sample.front().virtual_node_id.starts_with("node-1#type:"),
        .first_member = outputs.sample.empty() ? 0 : outputs.sample.front().member_ordinal,
        .first_port = outputs.sample.empty() ? 0 : outputs.sample.front().source.port_ordinal,
        .first_connected = !outputs.sample.empty()
            && outputs.sample.front().has_existing_downstream_connection,
    };
}

consteval VirtualOutputSnapshot event_output_snapshot()
{
    GraphBuilder g;
    auto source = _annotate_node_source_info(
        g.node<EventConcatenation>(1, EventTypeId::empty).node_ref(),
        "event-source");
    auto sink = g.node<DummyEventSink>();
    sink.connect_event_input(0, source.event_port());
    auto const outputs = g.virtual_outputs();
    return {
        .sample_count = outputs.sample.size(),
        .event_count = outputs.event.size(),
        .id_prefix = !outputs.event.empty()
            && outputs.event.front().virtual_node_id.starts_with("event-source#type:"),
        .first_connected = !outputs.event.empty()
            && outputs.event.front().has_existing_downstream_connection,
    };
}

consteval bool public_event_input_connected()
{
    GraphBuilder g;
    auto input = g.event_input(EventTypeId::empty);
    auto sink = g.node<DummyEventSink>();
    sink.connect_event_input(0, input);
    return g.public_event_input_is_connected(0);
}

consteval VirtualOutputSnapshot shared_virtual_output_snapshot()
{
    GraphBuilder g;
    auto a = _annotate_node_source_info(
        g.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(), "shared");
    auto b = _annotate_node_source_info(
        g.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(), "shared");
    (void)a;
    (void)b;
    auto const outputs = g.virtual_outputs();
    return {
        .sample_count = outputs.sample.size(),
        .id_prefix = outputs.sample.size() >= 1
            && outputs.sample[0].virtual_node_id.starts_with("shared#type:"),
        .first_member = outputs.sample.empty() ? 0 : outputs.sample[0].member_ordinal,
        .shared_id = outputs.sample.size() == 2
            && outputs.sample[0].virtual_node_id == outputs.sample[1].virtual_node_id,
        .distinct_members = outputs.sample.size() == 2
            && outputs.sample[0].member_ordinal != outputs.sample[1].member_ordinal,
    };
}

struct FamilySnapshot {
    size_t family_count = 0;
    bool id_prefix = false;
    size_t family_ordinal = 0;
    ChannelTypeId channel_type = ChannelTypeId::mono;
    size_t channel_count = 0;
    size_t left_sources = 0;
    size_t right_sources = 0;
    bool left_connected = false;
    bool right_connected = false;
};

consteval FamilySnapshot stereo_family_snapshot(bool connect_left)
{
    GraphBuilder g;
    auto source = g.node<Sum<stereo, SampleStreamLayout::planar, 1>>();
    auto annotated = _annotate_node_source_info(source.node_ref(), "stereo");
    (void)annotated;
    if (connect_left) {
        auto sink = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
        sink(source.static_output<0>()[stereo::left]);
    }
    auto const families = g.virtual_sample_output_families();
    FamilySnapshot result{.family_count = families.families.size()};
    if (families.families.empty()) return result;
    auto const& family = families.families.front();
    result.id_prefix = family.virtual_node_id.starts_with("stereo#type:");
    result.family_ordinal = family.family_ordinal;
    result.channel_type = family.channel_type;
    result.channel_count = family.channels.size();
    if (family.channels.size() >= 2) {
        result.left_sources = family.channels[0].sources.size();
        result.right_sources = family.channels[1].sources.size();
        result.left_connected = family.channels[stereo::left.channel_ordinal]
            .has_existing_downstream_connection;
        result.right_connected = family.channels[stereo::right.channel_ordinal]
            .has_existing_downstream_connection;
    }
    return result;
}

consteval VirtualPortConnectivity stereo_metadata_connectivity()
{
    GraphBuilder g;
    auto source = g.node<Sum<stereo, SampleStreamLayout::planar, 1>>();
    auto annotated = _annotate_node_source_info(source.node_ref(), "stereo");
    (void)annotated;
    auto sink = g.node<Sum<mono, SampleStreamLayout::planar, 1>>();
    sink(source.static_output<0>()[stereo::left]);
    auto const metadata = g.build_metadata();
    for (auto const& node : metadata.virtual_nodes) {
        if (node.source_identity == "stereo" && !node.sample_outputs.empty())
            return node.sample_outputs.front().connectivity;
    }
    return VirtualPortConnectivity::disconnected;
}

struct TypedIdentitySnapshot {
    bool sample_matches_single = false;
    bool event_is_distinct = false;
};

consteval TypedIdentitySnapshot typed_identity_snapshot()
{
    GraphBuilder single;
    auto single_sum = _annotate_node_source_info(
        single.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(),
        "shared");
    (void)single_sum;
    auto const single_id = single.virtual_outputs().sample.front().virtual_node_id;

    GraphBuilder split;
    auto split_sum = _annotate_node_source_info(
        split.node<Sum<mono, SampleStreamLayout::planar, 1>>().node_ref(),
        "shared");
    auto split_events = _annotate_node_source_info(
        split.node<EventConcatenation>(1, EventTypeId::empty).node_ref(),
        "shared");
    (void)split_sum;
    (void)split_events;
    auto const outputs = split.virtual_outputs();
    return {
        .sample_matches_single = outputs.sample.size() == 1
            && outputs.sample.front().virtual_node_id == single_id,
        .event_is_distinct = outputs.sample.size() == 1
            && outputs.event.size() == 1
            && outputs.sample.front().virtual_node_id
                != outputs.event.front().virtual_node_id,
    };
}

struct PublicOutputSnapshot {
    size_t graph_output_count = 0;
    bool name_main = false;
    ChannelTypeId channel_type = ChannelTypeId::mono;
    SampleStreamLayout sample_layout = SampleStreamLayout::planar;
    size_t family_count = 0;
    size_t family_channels = 0;
    bool left_maps_to_zero = false;
    bool right_maps_to_zero = false;
};

consteval PublicOutputSnapshot named_channel_output_snapshot()
{
    GraphBuilder g;
    g.outputs("main"_P[stereo::left] = 0.0f,
              "main"_P[stereo::right] = 0.0f);
    auto const built = g.build_root_node();
    auto const families = g.public_sample_output_families();
    PublicOutputSnapshot result{
        .graph_output_count = built.graph.outputs().size(),
        .name_main = built.graph.outputs().size() == 1
            && built.graph.outputs()[0].name == "main",
        .channel_type = built.graph.outputs().size() == 1
            ? built.graph.outputs()[0].channel_layout.channel_type
            : ChannelTypeId::mono,
        .family_count = families.families.size(),
    };
    if (!families.families.empty()) {
        auto const& family = families.families.front();
        result.family_channels = family.channels.size();
        result.left_maps_to_zero = family.channels.size() >= 1
            && family.channels[0].port_ordinals.size() == 1
            && family.channels[0].port_ordinals[0] == 0;
        result.right_maps_to_zero = family.channels.size() >= 2
            && family.channels[1].port_ordinals.size() == 1
            && family.channels[1].port_ordinals[0] == 0;
    }
    return result;
}

consteval PublicOutputSnapshot repeated_named_output_snapshot()
{
    GraphBuilder g;
    g.outputs("main"_P = 0.25f);
    g.outputs("main"_P = 0.5f);
    auto const built = g.build_root_node();
    auto const families = g.public_sample_output_families();
    return {
        .graph_output_count = built.graph.outputs().size(),
        .name_main = built.graph.outputs().size() == 1
            && built.graph.outputs().front().name == "main",
        .channel_type = built.graph.outputs().size() == 1
            ? built.graph.outputs().front().channel_layout.channel_type
            : ChannelTypeId::stereo,
        .family_count = families.families.size(),
        .family_channels = families.families.empty()
            ? 0 : families.families.front().channels.size(),
        .left_maps_to_zero = !families.families.empty()
            && !families.families.front().channels.empty()
            && families.families.front().channels[0].port_ordinals.size() == 1
            && families.families.front().channels[0].port_ordinals[0] == 0,
    };
}

consteval PublicOutputSnapshot repeated_unnamed_output_snapshot()
{
    GraphBuilder g;
    g.outputs(0.25f);
    g.outputs(0.5f);
    auto const built = g.build_root_node();
    auto const families = g.public_sample_output_families();
    return {
        .graph_output_count = built.graph.outputs().size(),
        .name_main = built.graph.outputs().size() == 1
            && built.graph.outputs().front().name == "main",
        .channel_type = built.graph.outputs().size() == 1
            ? built.graph.outputs().front().channel_layout.channel_type
            : ChannelTypeId::stereo,
        .family_count = families.families.size(),
        .family_channels = families.families.empty()
            ? 0 : families.families.front().channels.size(),
        .left_maps_to_zero = !families.families.empty()
            && !families.families.front().channels.empty()
            && families.families.front().channels[0].port_ordinals.size() == 1
            && families.families.front().channels[0].port_ordinals[0] == 0,
    };
}

consteval PublicOutputSnapshot whole_and_channel_output_snapshot()
{
    GraphBuilder g;
    auto stereo_source = g.node<Sum<stereo, SampleStreamLayout::interleaved, 1>>();
    g.outputs("main"_P = stereo_source);
    g.outputs("main"_P[stereo::left] = 0.25f);
    auto const built = g.build_root_node();
    auto const families = g.public_sample_output_families();
    PublicOutputSnapshot result{
        .graph_output_count = built.graph.outputs().size(),
        .name_main = built.graph.outputs().size() == 1
            && built.graph.outputs().front().name == "main",
        .channel_type = built.graph.outputs().size() == 1
            ? built.graph.outputs().front().channel_layout.channel_type
            : ChannelTypeId::mono,
        .sample_layout = built.graph.outputs().size() == 1
            ? built.graph.outputs().front().channel_layout.sample_layout
            : SampleStreamLayout::interleaved,
        .family_count = families.families.size(),
    };
    if (!families.families.empty()) {
        auto const& channels = families.families.front().channels;
        result.family_channels = channels.size();
        result.left_maps_to_zero = channels.size() >= 1
            && channels[0].port_ordinals.size() == 1
            && channels[0].port_ordinals[0] == 0;
        result.right_maps_to_zero = channels.size() >= 2
            && channels[1].port_ordinals.size() == 1
            && channels[1].port_ordinals[0] == 0;
    }
    return result;
}

consteval bool named_channel_contributions_share_family()
{
    GraphBuilder g;
    g.outputs("main"_P[stereo::left] = 0.25f,
              "main"_P[stereo::right] = 0.5f);
    auto const built = g.build_root_node();
    return built.graph.outputs().size() == 1
        && built.graph.outputs().front().name == "main";
}

consteval bool functional_subgraph_has_main_output()
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
    return built.graph.outputs().size() == 1
        && built.graph.outputs().front().name == "main";
}

consteval bool nested_functional_subgraphs_compile()
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
    (void)g.build_root_node();
    return true;
}

} // namespace

TEST(GraphVirtualOutputsTest, EnumeratesVirtualNodeOutputPorts)
{
    constexpr auto s = single_sample_output_snapshot(false);
    EXPECT_EQ(s.sample_count, 1u);
    EXPECT_TRUE(s.id_prefix);
    EXPECT_EQ(s.first_member, 0u);
    EXPECT_EQ(s.first_port, 0u);
    EXPECT_FALSE(s.first_connected);
}

TEST(GraphVirtualOutputsTest, ReportsExistingDownstreamConnection)
{
    constexpr auto s = single_sample_output_snapshot(true);
    EXPECT_EQ(s.sample_count, 1u);
    EXPECT_TRUE(s.first_connected);
}

TEST(GraphVirtualOutputsTest, ReportsAuthoredEventDownstreamConnection)
{
    constexpr auto s = event_output_snapshot();
    EXPECT_EQ(s.event_count, 1u);
    EXPECT_TRUE(s.id_prefix);
    EXPECT_TRUE(s.first_connected);
}

TEST(GraphVirtualOutputsTest, PublicEventInputConnectivityUsesAuthoredConnections)
{
    static_assert(public_event_input_connected());
    EXPECT_TRUE(public_event_input_connected());
}

TEST(GraphVirtualOutputsTest, GroupsConcreteMembersOfSharedVirtualNode)
{
    constexpr auto s = shared_virtual_output_snapshot();
    EXPECT_EQ(s.sample_count, 2u);
    EXPECT_TRUE(s.shared_id);
    EXPECT_TRUE(s.id_prefix);
    EXPECT_TRUE(s.distinct_members);
}

TEST(GraphVirtualOutputsTest, GroupsStereoChannelOutputsIntoOneFamily)
{
    constexpr auto s = stereo_family_snapshot(false);
    EXPECT_EQ(s.family_count, 1u);
    EXPECT_TRUE(s.id_prefix);
    EXPECT_EQ(s.family_ordinal, 0u);
    EXPECT_EQ(s.channel_type, ChannelTypeId::stereo);
    EXPECT_EQ(s.channel_count, 2u);
    EXPECT_EQ(s.left_sources, 1u);
    EXPECT_EQ(s.right_sources, 1u);
}

TEST(GraphVirtualOutputsTest, ReportsAuthoredStereoChannelConnectivity)
{
    constexpr auto s = stereo_family_snapshot(true);
    EXPECT_TRUE(s.left_connected);
    EXPECT_FALSE(s.right_connected);
}

TEST(GraphVirtualOutputsTest, MetadataReportsMixedAuthoredStereoConnectivity)
{
    static_assert(stereo_metadata_connectivity() == VirtualPortConnectivity::mixed);
    EXPECT_EQ(stereo_metadata_connectivity(), VirtualPortConnectivity::mixed);
}

TEST(GraphVirtualOutputsTest, TypedIdentityDoesNotChangeWhenAnotherTypeSharesTheSource)
{
    constexpr auto s = typed_identity_snapshot();
    EXPECT_TRUE(s.sample_matches_single);
    EXPECT_TRUE(s.event_is_distinct);
}

TEST(GraphVirtualOutputsTest, NamedChannelOutputsKeepNameAndChannelAsSeparateIdentity)
{
    constexpr auto s = named_channel_output_snapshot();
    EXPECT_EQ(s.graph_output_count, 1u);
    EXPECT_TRUE(s.name_main);
    EXPECT_EQ(s.channel_type, ChannelTypeId::stereo);
    EXPECT_EQ(s.family_count, 1u);
    EXPECT_EQ(s.family_channels, 2u);
    EXPECT_TRUE(s.left_maps_to_zero);
    EXPECT_TRUE(s.right_maps_to_zero);
}

TEST(GraphVirtualOutputsTest, RepeatedNamedPublicOutputsShareOneSummedGraphPort)
{
    constexpr auto s = repeated_named_output_snapshot();
    EXPECT_EQ(s.graph_output_count, 1u);
    EXPECT_TRUE(s.name_main);
    EXPECT_EQ(s.channel_type, ChannelTypeId::mono);
    EXPECT_EQ(s.family_count, 1u);
    EXPECT_TRUE(s.left_maps_to_zero);
}

TEST(GraphVirtualOutputsTest, RepeatedUnnamedPublicOutputsShareTheMainGraphPort)
{
    constexpr auto s = repeated_unnamed_output_snapshot();
    EXPECT_EQ(s.graph_output_count, 1u);
    EXPECT_TRUE(s.name_main);
    EXPECT_EQ(s.channel_type, ChannelTypeId::mono);
    EXPECT_EQ(s.family_count, 1u);
    EXPECT_TRUE(s.left_maps_to_zero);
}

TEST(GraphVirtualOutputsTest, NamedChannelOutputsFormOneNamedStereoFamily)
{
    constexpr auto s = named_channel_output_snapshot();
    EXPECT_EQ(s.graph_output_count, 1u);
    EXPECT_TRUE(s.name_main);
    EXPECT_EQ(s.channel_type, ChannelTypeId::stereo);
}

TEST(GraphVirtualOutputsTest, WholeStreamAndChannelContributorsShareOneTypedPublicOutput)
{
    constexpr auto s = whole_and_channel_output_snapshot();
    EXPECT_EQ(s.graph_output_count, 1u);
    EXPECT_TRUE(s.name_main);
    EXPECT_EQ(s.channel_type, ChannelTypeId::stereo);
    EXPECT_EQ(s.sample_layout, SampleStreamLayout::planar);
    EXPECT_EQ(s.family_count, 1u);
    EXPECT_TRUE(s.left_maps_to_zero);
    EXPECT_TRUE(s.right_maps_to_zero);
}

TEST(GraphVirtualOutputsTest, NamedChannelOutputContributionsShareTheirStereoFamily)
{
    static_assert(named_channel_contributions_share_family());
    EXPECT_TRUE(named_channel_contributions_share_family());
}

TEST(GraphVirtualOutputsTest, ChannelQualifiedDescriptorsAreNotNodeArguments)
{
    using ChannelArgument = decltype("frequency"_P[mono::center] = 0.25f);
    static_assert(!details::valid_node_call_args_v<ChannelArgument>);
}

TEST(GraphVirtualOutputsTest, ChannelPortsSupportConstexprEquality)
{
    constexpr auto is_left = []<auto channel>() {
        if constexpr (channel == stereo::left) return true;
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
    static_assert(functional_subgraph_has_main_output());
    EXPECT_TRUE(functional_subgraph_has_main_output());
}

TEST(GraphVirtualOutputsTest, FunctionalSubgraphsCanNestWithoutAmbientScopeState)
{
    static_assert(nested_functional_subgraphs_compile());
    EXPECT_TRUE(nested_functional_subgraphs_compile());
}

} // namespace iv
