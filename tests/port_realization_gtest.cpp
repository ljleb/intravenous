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

struct SampleSource {
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("out")};
    }
    void tick_block(iv::TickBlockContext<SampleSource> const&) const {}
};

struct RateDoubler {
    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("in")};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("out")};
    }
    void constrain_ports(iv::ConstrainPortsContext<RateDoubler>& ctx) const
    {
        ctx.input<"in">().pace() = 1;
        ctx.output<"out">().pace() = 2;
    }
    void tick_block(iv::TickBlockContext<RateDoubler> const&) const {}
};

struct OrdinaryIdentity {
    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("in")};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("out")};
    }
    void tick_block(iv::TickBlockContext<OrdinaryIdentity> const&) const {}
};

struct FftSizedOutput {
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "out",
            iv::SampleOutputProperties{
                .value_types = iv::continuous_value_types<iv::FFTBlock>,
            })};
    }
    void constrain_ports(iv::ConstrainPortsContext<FftSizedOutput>& ctx) const
    {
        ctx.output<"out">().size() = 513;
    }
    void tick_block(iv::TickBlockContext<FftSizedOutput> const&) const {}
};

struct InvalidFftSizedOutput {
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "out",
            iv::SampleOutputProperties{
                .value_types = iv::continuous_value_types<iv::FFTBlock>,
            })};
    }
    void constrain_ports(
        iv::ConstrainPortsContext<InvalidFftSizedOutput>& ctx) const
    {
        ctx.output<"out">().size() = 1;
    }
    void tick_block(iv::TickBlockContext<InvalidFftSizedOutput> const&) const {}
};

struct UnsupportedDyadicPace {
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("out")};
    }
    void constrain_ports(
        iv::ConstrainPortsContext<UnsupportedDyadicPace>& ctx) const
    {
        ctx.output<"out">().pace() = iv::PortPace::from_exponent(65);
    }
    void tick_block(iv::TickBlockContext<UnsupportedDyadicPace> const&) const {}
};

struct TimedPorts {
    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("in")};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("out")};
    }
    void constrain_ports(iv::ConstrainPortsContext<TimedPorts>& ctx) const
    {
        ctx.input<"in">().history() = 7;
        ctx.output<"out">().history() = 3;
        ctx.output<"out">().latency() = 2;
    }
    void tick_block(iv::TickBlockContext<TimedPorts> const&) const {}
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
    static_assert(std::same_as<
        iv::ContinuousValueType<iv::Sample>::storage_element,
        iv::Sample>);
    static_assert(std::same_as<
        iv::ContinuousValueType<iv::FractionalGlobalIndex>::storage_element,
        iv::FractionalGlobalIndex>);
    static_assert(std::same_as<
        iv::ContinuousValueType<iv::FFTBlock>::storage_element,
        iv::FFTBin>);
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
    auto const& fft =
        iv::continuous_value_type_info(iv::ContinuousValueTypeId::fft_block);
    EXPECT_EQ(fft.storage_element_size, sizeof(iv::FFTBin));
    EXPECT_TRUE(fft.valid_size(513));
    EXPECT_FALSE(fft.valid_size(1));
    EXPECT_EQ(fft.sample_support(513), 1024u);
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
                .kind = iv::PortKind::sample,
                .port_index = 0,
            },
            .second = {
                .direction = iv::PortConstraintDirection::input,
                .kind = iv::PortKind::sample,
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

TEST(PortRealization, ResolvesDynamicContinuousValueSize)
{
    iv::ConfiguredGraph graph;
    auto const node = append(graph, FftSizedOutput{});
    auto realized = iv::graph_jit::resolve_port_realization(graph);
    if (!realized) FAIL() << realized.error();
    EXPECT_EQ(realized->output(node, 0).size, 513u);
    EXPECT_EQ(realized->output(node, 0).sample_support, 1024u);
}

TEST(PortRealization, RejectsFftBlockWithoutDcAndNyquistBins)
{
    iv::ConfiguredGraph graph;
    append(graph, InvalidFftSizedOutput{});
    auto realized = iv::graph_jit::resolve_port_realization(graph);
    ASSERT_FALSE(realized.has_value());
    EXPECT_NE(realized.error().find("invalid size for FFTBlock"), std::string::npos);
}

TEST(PortRealization, PortPaceUsesPowerOfTwoFactorsAndSignedSolvedExponents)
{
    EXPECT_EQ(iv::PortPace(1).exponent(), 0);
    EXPECT_EQ(iv::PortPace(512).exponent(), 9);
    EXPECT_EQ(iv::PortPace::from_exponent(-9).exponent(), -9);
    EXPECT_THROW(static_cast<void>(iv::PortPace(3)), std::invalid_argument);
}

TEST(PortRealization, RejectsPaceOutsideQ64Point64ExecutionDomain)
{
    iv::ConfiguredGraph graph;
    append(graph, UnsupportedDyadicPace{});
    auto realized = iv::graph_jit::resolve_port_realization(graph);
    ASSERT_FALSE(realized.has_value());
    EXPECT_NE(realized.error().find("-64 through +64"), std::string::npos);
}

TEST(PortRealization, ResolvesHistoryAndLatencyFromInstanceConstraints)
{
    iv::ConfiguredGraph graph;
    auto const node = append(graph, TimedPorts{});
    auto realized = iv::graph_jit::resolve_port_realization(graph);
    if (!realized) FAIL() << realized.error();
    EXPECT_EQ(realized->input(node, 0).history, 7u);
    EXPECT_EQ(realized->output(node, 0).history, 3u);
    EXPECT_EQ(realized->output(node, 0).latency, 2u);
}

TEST(PortRealization, ConnectionsRelateAbsoluteRatesNotLocalPaceConstants)
{
    iv::ConfiguredGraph graph;
    auto const source = append(graph, SampleSource{});
    auto const doubler = append(graph, RateDoubler{});
    auto const downstream = append(graph, OrdinaryIdentity{});
    graph.connections.record_configured_sample_connection(
        mono_connection(source, 0, doubler, 0));
    graph.connections.record_configured_sample_connection(
        mono_connection(doubler, 0, downstream, 0));

    auto realized = iv::graph_jit::resolve_port_realization(graph);
    if (!realized) FAIL() << realized.error();
    EXPECT_EQ(realized->execution_rate(source), iv::PortPace(1));
    EXPECT_EQ(realized->execution_rate(doubler), iv::PortPace(1));
    EXPECT_EQ(realized->execution_rate(downstream), iv::PortPace(2));
    EXPECT_EQ(realized->output(doubler, 0).local_pace, iv::PortPace(2));
    EXPECT_EQ(realized->output(doubler, 0).absolute_pace, iv::PortPace(2));
    EXPECT_EQ(realized->output(doubler, 0).sample_support, 1u);
    EXPECT_EQ(realized->input(downstream, 0).local_pace, iv::PortPace(1));
    EXPECT_EQ(realized->input(downstream, 0).absolute_pace, iv::PortPace(2));
}

} // namespace
