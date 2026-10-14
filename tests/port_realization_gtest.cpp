#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph_jit/port_realization.h>
#include <intravenous/node/tick.h>

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace {

struct MixedIdentity {
    static constexpr auto domain =
        iv::continuous_value_types<iv::Sample, iv::FractionalGlobalIndex>;

    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input(
            "in", iv::SampleInputProperties{.value_types = domain})};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "out", iv::SampleOutputProperties{.value_types = domain})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<MixedIdentity>& context) const
    {
        context.output<"out">().value_type()
            = context.input<"in">().value_type();
    }

    void tick_block(iv::TickBlockContext<MixedIdentity> const&) const {}
};

struct FractionalSource {
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "out",
            iv::SampleOutputProperties{
                .value_types =
                    iv::continuous_value_types<iv::FractionalGlobalIndex>,
            })};
    }

    void tick_block(iv::TickBlockContext<FractionalSource> const&) const {}
};

struct FractionalOnly {
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "out",
            iv::SampleOutputProperties{
                .value_types =
                    iv::continuous_value_types<iv::FractionalGlobalIndex>,
            })};
    }

    void tick_block(iv::TickBlockContext<FractionalOnly> const&) const {}
};

struct AnchoredChoice {
    iv::ContinuousValueTypeId selected =
        iv::continuous_value_type_id_v<iv::Sample>;

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "out",
            iv::SampleOutputProperties{
                .value_types = MixedIdentity::domain,
            })};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<AnchoredChoice>& context) const
    {
        context.output<"out">().value_type() = selected;
    }

    void tick_block(iv::TickBlockContext<AnchoredChoice> const&) const {}
};

template<class Node>
iv::NodeBundleHandle append(iv::ConfiguredGraph& graph, Node const& node = {})
{
    return graph.node_bundles.append_concrete(
        iv::GraphBuilderNodeBundles::make_concrete_node(
            iv::details::reflect_node(node)));
}

iv::ConfiguredSampleConnection mono_connection(
    iv::NodeBundleHandle source,
    std::size_t source_port,
    iv::NodeBundleHandle target,
    std::size_t target_port)
{
    return {
        .source_type = iv::ChannelTypeId::mono,
        .source_channels = {{source, source_port, 0}},
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {{target, target_port, 0}},
    };
}

TEST(ContinuousValueRegistry, DefinesInitialClosedTypeCapabilities)
{
    static_assert(iv::ContinuousValueType<iv::Sample>::rank == 0);
    static_assert(
        iv::ContinuousValueType<iv::FractionalGlobalIndex>::channel_capability
        == iv::ContinuousValueChannelCapability::scalar);
    static_assert(iv::ContinuousValueType<iv::FFTBlock>::rank == 1);
    static_assert(iv::continuous_value_types<iv::Sample>.contains<iv::Sample>());
    static_assert(
        !iv::continuous_value_types<iv::Sample>
             .contains<iv::FractionalGlobalIndex>());
    EXPECT_EQ(
        iv::continuous_value_type_info(iv::ContinuousValueTypeId::sample)
            .channel_capability,
        iv::ContinuousValueChannelCapability::audio);
    EXPECT_EQ(
        iv::continuous_value_type_info(
            iv::ContinuousValueTypeId::fractional_global_index)
            .channel_capability,
        iv::ContinuousValueChannelCapability::scalar);
    EXPECT_EQ(
        iv::continuous_value_type_info(iv::ContinuousValueTypeId::fft_block)
            .rank,
        1u);
}

TEST(PortRealization, CapturesInstanceAuthoredValueTypeEquality)
{
    auto description = iv::details::reflect_node(MixedIdentity{});
    ASSERT_EQ(description.port_constraints.value_type_equalities.size(), 1u);
    EXPECT_EQ(
        description.port_constraints.value_type_equalities.front(),
        (iv::ContinuousValueTypeEqualityConstraint{
            .first = {
                .direction = iv::PortConstraintDirection::output,
                .port_index = 0,
            },
            .second = {
                .direction = iv::PortConstraintDirection::input,
                .port_index = 0,
            },
        }));
}

TEST(PortRealization, ConnectivitySelectsOneDeclaredAlternativeTransitively)
{
    iv::ConfiguredGraph graph;
    auto const source = append(graph, FractionalSource{});
    auto const identity = append(graph, MixedIdentity{});
    graph.connections.record_configured_sample_connection(
        mono_connection(source, 0, identity, 0));

    auto realized = iv::graph_jit::resolve_port_realization(graph);
    if (!realized) FAIL() << realized.error();
    EXPECT_EQ(
        realized->output(source, 0).value_type,
        iv::ContinuousValueTypeId::fractional_global_index);
    EXPECT_EQ(
        realized->input(identity, 0).value_type,
        iv::ContinuousValueTypeId::fractional_global_index);
    EXPECT_EQ(
        realized->output(identity, 0).value_type,
        iv::ContinuousValueTypeId::fractional_global_index);
}

TEST(PortRealization, ConfiguredNodeInstanceAnchorsOneDeclaredAlternative)
{
    iv::ConfiguredGraph graph;
    auto const node = append(
        graph,
        AnchoredChoice{
            .selected =
                iv::ContinuousValueTypeId::fractional_global_index,
        });

    auto realized = iv::graph_jit::resolve_port_realization(graph);
    if (!realized) FAIL() << realized.error();
    EXPECT_EQ(
        realized->output(node, 0).value_type,
        iv::ContinuousValueTypeId::fractional_global_index);
}

TEST(PortRealization, PropagatesValueTypesAcrossSubgraphBoundaries)
{
    iv::ConfiguredGraph graph;
    auto const boundary = graph.node_bundles.append_boundary();
    graph.node_bundles.bundle(boundary).append_boundary_sample_input({
        .name = "input",
        .value_types = MixedIdentity::domain,
    });
    auto const child = append(graph, MixedIdentity{});
    graph.connections.record_configured_sample_connection(
        mono_connection(boundary, 0, child, 0));
    auto const subgraph = graph.node_bundles.append_subgraph(
        boundary, child, 1, "test-subgraph");
    auto const source = append(graph, FractionalSource{});
    graph.connections.record_configured_sample_connection(
        mono_connection(source, 0, subgraph, 0));

    auto realized = iv::graph_jit::resolve_port_realization(graph);
    if (!realized) FAIL() << realized.error();
    EXPECT_EQ(
        realized->input(subgraph, 0).value_type,
        iv::ContinuousValueTypeId::fractional_global_index);
    EXPECT_EQ(
        realized->output(boundary, 0).value_type,
        iv::ContinuousValueTypeId::fractional_global_index);
    EXPECT_EQ(
        realized->input(child, 0).value_type,
        iv::ContinuousValueTypeId::fractional_global_index);
    EXPECT_EQ(
        realized->output(child, 0).value_type,
        iv::ContinuousValueTypeId::fractional_global_index);
}

TEST(PortRealization, RejectsAValueTypeDomainLeftAmbiguousByTheGraph)
{
    iv::ConfiguredGraph graph;
    append(graph, MixedIdentity{});

    auto realized = iv::graph_jit::resolve_port_realization(graph);
    ASSERT_FALSE(realized.has_value());
    EXPECT_NE(realized.error().find("unresolved"), std::string::npos);
}

TEST(PortRealization, RejectsAnInstanceAnchorOutsideTheDeclaredDomain)
{
    iv::ConfiguredGraph graph;
    append(
        graph,
        AnchoredChoice{.selected = iv::ContinuousValueTypeId::fft_block});

    auto realized = iv::graph_jit::resolve_port_realization(graph);
    ASSERT_FALSE(realized.has_value());
    EXPECT_NE(realized.error().find("no compatible"), std::string::npos);
}

TEST(PortRealization, TilingConstrainsEveryContinuousPortToAudioTypes)
{
    iv::ConfiguredGraph graph;
    auto const left = append(graph, MixedIdentity{});
    auto const right = append(graph, MixedIdentity{});
    std::array const members{left, right};
    auto const tiled = graph.node_bundles.append_tiled(
        members,
        {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::planar,
        });

    auto realized = iv::graph_jit::resolve_port_realization(graph);
    if (!realized) FAIL() << realized.error();
    EXPECT_EQ(
        realized->input(tiled, 0).value_type,
        iv::ContinuousValueTypeId::sample);
    EXPECT_EQ(
        realized->output(tiled, 0).value_type,
        iv::ContinuousValueTypeId::sample);
    EXPECT_EQ(
        realized->input(left, 0).value_type,
        iv::ContinuousValueTypeId::sample);
    EXPECT_EQ(
        realized->output(right, 0).value_type,
        iv::ContinuousValueTypeId::sample);
}

TEST(PortRealization, RejectsTilingOfScalarOnlyContinuousPorts)
{
    iv::ConfiguredGraph graph;
    auto const left = append(graph, FractionalOnly{});
    auto const right = append(graph, FractionalOnly{});
    std::array const members{left, right};
    graph.node_bundles.append_tiled(
        members,
        {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::planar,
        });

    auto realized = iv::graph_jit::resolve_port_realization(graph);
    ASSERT_FALSE(realized.has_value());
    EXPECT_NE(realized.error().find("tiling"), std::string::npos);
}

} // namespace
