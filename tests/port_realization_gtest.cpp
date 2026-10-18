#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph_jit/port_realization.h>
#include <intravenous/node/tick.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

struct MixedIdentity {
    static constexpr auto domain =
        iv::continuous_value_types<iv::Sample, iv::FractionalGlobalIndex>;

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample, iv::FractionalGlobalIndex>(
            "in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample, iv::FractionalGlobalIndex>(
            "out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::FractionalGlobalIndex>(
            "out", iv::OutputProduction::tick)};
    }

    void tick_block(iv::TickBlockContext<FractionalSource> const&) const {}
};

struct FractionalOnly {
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::FractionalGlobalIndex>(
            "out", iv::OutputProduction::tick)};
    }

    void tick_block(iv::TickBlockContext<FractionalOnly> const&) const {}
};

struct AnchoredChoice {
    iv::ContinuousValueTypeId selected =
        iv::continuous_value_type_id_v<iv::Sample>;

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample, iv::FractionalGlobalIndex>(
            "out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }
    void tick_block(iv::TickBlockContext<SampleSource> const&) const {}
};

struct RateDoubler {
    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }
    void constrain_ports(iv::ConstrainPortsContext<RateDoubler>& ctx) const
    {
        ctx.input<"in">().pace() = 1;
        ctx.output<"out">().pace() = 2;
    }
    void tick_block(iv::TickBlockContext<RateDoubler> const&) const {}
};

struct AbsolutelyAnchoredRateDoubler {
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
    }
    void constrain_ports(
        iv::ConstrainPortsContext<AbsolutelyAnchoredRateDoubler>& ctx) const
    {
        ctx.output<"out">().pace() = 2;
        ctx.output<"out">().pace().absolute() = 8;
    }
    void tick_block(
        iv::TickBlockContext<AbsolutelyAnchoredRateDoubler> const&) const {}
};

struct AbsolutePaceAnchor {
    std::uint64_t factor = 1;

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "in", iv::InputAccess::sequential)};
    }
    void constrain_ports(
        iv::ConstrainPortsContext<AbsolutePaceAnchor>& ctx) const
    {
        ctx.input<"in">().pace().absolute() = factor;
    }
    void tick_block(iv::TickBlockContext<AbsolutePaceAnchor> const&) const {}
};

struct OrdinaryIdentity {
    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }
    void tick_block(iv::TickBlockContext<OrdinaryIdentity> const&) const {}
};

struct FftSizedOutput {
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::FFTBlock>(
            "out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::FFTBlock>(
            "out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }
    void constrain_ports(iv::ConstrainPortsContext<TimedPorts>& ctx) const
    {
        ctx.input<"in">().history() = 7;
        ctx.output<"out">().history() = 3;
        ctx.output<"out">().latency() = 2;
    }
    void tick_block(iv::TickBlockContext<TimedPorts> const&) const {}
};

struct PaceRelation {
    std::uint64_t input_factor = 1;
    std::uint64_t output_factor = 1;
    std::uint64_t absolute_input_factor = 0;

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
    }

    void constrain_ports(iv::ConstrainPortsContext<PaceRelation>& ctx) const
    {
        ctx.input<"in">().pace() = input_factor;
        ctx.output<"out">().pace() = output_factor;
        if (absolute_input_factor != 0)
            ctx.input<"in">().pace().absolute() = absolute_input_factor;
    }

    void tick_block(iv::TickBlockContext<PaceRelation> const&) const {}
};

struct MixedCoverageOrderingSource {
    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>(
                "samples", iv::OutputProduction::coverage),
            iv::event_output(
                "events", iv::EventTypeId::trigger,
                iv::OutputProduction::coverage),
        };
    }

    void tick_block(
        iv::TickBlockContext<MixedCoverageOrderingSource> const&) const {}
    void tock_coverage(
        iv::TockCoverageContext<MixedCoverageOrderingSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            MixedCoverageOrderingSource>&) const {}
};

struct MixedCoverageOrderingRelay {
    static constexpr auto inputs()
    {
        return std::tuple{
            iv::input<iv::Sample>(
                "samples", iv::InputAccess::coverage),
            iv::event_input(
                "events", iv::EventTypeId::trigger,
                iv::InputAccess::coverage),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>(
                "samples", iv::OutputProduction::coverage),
            iv::event_output(
                "events", iv::EventTypeId::trigger,
                iv::OutputProduction::coverage),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<MixedCoverageOrderingRelay>& ctx) const
    {
        ctx.output<"samples">().ordering()
            = ctx.input<"samples">().ordering();
        ctx.output<"events">().ordering()
            = ctx.input<"events">().ordering();
    }

    void tick_block(
        iv::TickBlockContext<MixedCoverageOrderingRelay> const&) const {}
    void tock_coverage(
        iv::TockCoverageContext<MixedCoverageOrderingRelay>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            MixedCoverageOrderingRelay>&) const {}
    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<
            MixedCoverageOrderingRelay>&) const {}
};

struct MixedSortedCoverageSink {
    static constexpr auto inputs()
    {
        return std::tuple{
            iv::input<iv::Sample>(
                "samples", iv::InputAccess::coverage),
            iv::event_input(
                "events", iv::EventTypeId::trigger,
                iv::InputAccess::coverage),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<MixedSortedCoverageSink>& ctx) const
    {
        ctx.input<"samples">().ordering() = iv::CoverageOrdering::sorted;
        ctx.input<"events">().ordering() = iv::CoverageOrdering::sorted;
    }

    void tick_block(
        iv::TickBlockContext<MixedSortedCoverageSink> const&) const {}
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
        .access = iv::default_sequential_input_properties(MixedIdentity::domain),
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

TEST(PortRealization, AbsolutePaceAnchorFixesTheComponentScale)
{
    auto description = iv::details::reflect_node(
        AbsolutelyAnchoredRateDoubler{});
    ASSERT_EQ(
        description.port_constraints.absolute_pace_anchors.size(), 1u);
    EXPECT_EQ(
        description.port_constraints.absolute_pace_anchors.front().value,
        iv::PortPace(8));

    iv::ConfiguredGraph graph;
    auto const source = append(graph, AbsolutelyAnchoredRateDoubler{});
    auto const downstream = append(graph, OrdinaryIdentity{});
    graph.connections.record_configured_sample_connection(
        mono_connection(source, 0, downstream, 0));

    auto realized = iv::graph_jit::resolve_port_realization(graph);
    if (!realized) FAIL() << realized.error();
    EXPECT_EQ(realized->execution_rate(source), iv::PortPace(4));
    EXPECT_EQ(realized->output(source, 0).local_pace, iv::PortPace(2));
    EXPECT_EQ(realized->output(source, 0).absolute_pace, iv::PortPace(8));
    EXPECT_EQ(realized->execution_rate(downstream), iv::PortPace(8));
    EXPECT_EQ(realized->input(downstream, 0).absolute_pace, iv::PortPace(8));
}

TEST(PortRealization, RejectsConflictingAbsolutePaceAnchors)
{
    iv::ConfiguredGraph graph;
    auto const source = append(graph, AbsolutelyAnchoredRateDoubler{});
    auto const target = append(graph, AbsolutePaceAnchor{.factor = 4});
    graph.connections.record_configured_sample_connection(
        mono_connection(source, 0, target, 0));

    auto realized = iv::graph_jit::resolve_port_realization(graph);
    ASSERT_FALSE(realized.has_value());
    EXPECT_NE(
        realized.error().find("absolute pace anchor"),
        std::string::npos);
}

TEST(PortRealization, RejectsInconsistentWeightedPaceCycleInEveryInsertionOrder)
{
    constexpr std::array permutations{
        std::array<std::size_t, 3>{0, 1, 2},
        std::array<std::size_t, 3>{0, 2, 1},
        std::array<std::size_t, 3>{1, 0, 2},
        std::array<std::size_t, 3>{1, 2, 0},
        std::array<std::size_t, 3>{2, 0, 1},
        std::array<std::size_t, 3>{2, 1, 0},
    };

    for (auto const& order : permutations) {
        SCOPED_TRACE(
            std::to_string(order[0]) + std::to_string(order[1])
            + std::to_string(order[2]));
        iv::ConfiguredGraph graph;
        auto const first = append(graph, PaceRelation{1, 2});
        auto const second = append(graph, PaceRelation{1, 2});
        auto const third = append(graph, PaceRelation{1, 2});
        std::array const connections{
            mono_connection(first, 0, second, 0),
            mono_connection(second, 0, third, 0),
            mono_connection(third, 0, first, 0),
        };
        for (auto const connection : order) {
            graph.connections.record_configured_sample_connection(
                connections[connection]);
        }

        auto realized = iv::graph_jit::resolve_port_realization(graph);
        ASSERT_FALSE(realized.has_value());
        EXPECT_NE(
            realized.error().find("inconsistent pace cycle"),
            std::string::npos);
    }
}

TEST(PortRealization, ConsistentWeightedPaceCycleIsInsertionOrderInvariant)
{
    constexpr std::array permutations{
        std::array<std::size_t, 3>{0, 1, 2},
        std::array<std::size_t, 3>{0, 2, 1},
        std::array<std::size_t, 3>{1, 0, 2},
        std::array<std::size_t, 3>{1, 2, 0},
        std::array<std::size_t, 3>{2, 0, 1},
        std::array<std::size_t, 3>{2, 1, 0},
    };
    using Snapshot = std::array<iv::PortPace::exponent_type, 9>;
    std::optional<Snapshot> reference;

    for (auto const& node_order : permutations) {
        for (auto const& connection_order : permutations) {
            SCOPED_TRACE(
                "nodes " + std::to_string(node_order[0])
                + std::to_string(node_order[1])
                + std::to_string(node_order[2]) + ", connections "
                + std::to_string(connection_order[0])
                + std::to_string(connection_order[1])
                + std::to_string(connection_order[2]));
            iv::ConfiguredGraph graph;
            // The three connection differences are -1, 0 and +1. Their sum
            // is zero, but the union-find paths include a negative relative
            // exponent and are compressed in different directions.
            constexpr std::array relations{
                PaceRelation{1, 2},
                PaceRelation{4, 1},
                PaceRelation{1, 2},
            };
            std::array<iv::NodeBundleHandle, 3> nodes{};
            for (auto const role : node_order)
                nodes[role] = append(graph, relations[role]);
            auto const [first, second, third] = nodes;
            std::array const connections{
                mono_connection(first, 0, second, 0),
                mono_connection(second, 0, third, 0),
                mono_connection(third, 0, first, 0),
            };
            for (auto const connection : connection_order) {
                graph.connections.record_configured_sample_connection(
                    connections[connection]);
            }

            auto realized = iv::graph_jit::resolve_port_realization(graph);
            if (!realized) FAIL() << realized.error();

            EXPECT_EQ(realized->execution_rate(first), iv::PortPace(2));
            EXPECT_EQ(realized->execution_rate(second), iv::PortPace(1));
            EXPECT_EQ(realized->execution_rate(third), iv::PortPace(1));
            EXPECT_EQ(
                realized->output(first, 0).absolute_pace,
                realized->input(second, 0).absolute_pace);
            EXPECT_EQ(
                realized->output(second, 0).absolute_pace,
                realized->input(third, 0).absolute_pace);
            EXPECT_EQ(
                realized->output(third, 0).absolute_pace,
                realized->input(first, 0).absolute_pace);

            Snapshot const snapshot{
                realized->execution_rate(first).exponent(),
                realized->execution_rate(second).exponent(),
                realized->execution_rate(third).exponent(),
                realized->input(first, 0).absolute_pace.exponent(),
                realized->output(first, 0).absolute_pace.exponent(),
                realized->input(second, 0).absolute_pace.exponent(),
                realized->output(second, 0).absolute_pace.exponent(),
                realized->input(third, 0).absolute_pace.exponent(),
                realized->output(third, 0).absolute_pace.exponent(),
            };
            if (!reference) reference = snapshot;
            EXPECT_EQ(snapshot, *reference);
        }
    }
}

TEST(PortRealization, AbsolutePaceAnchorDominatesEveryWeightedCycleUnionOrder)
{
    constexpr std::array permutations{
        std::array<std::size_t, 3>{0, 1, 2},
        std::array<std::size_t, 3>{0, 2, 1},
        std::array<std::size_t, 3>{1, 0, 2},
        std::array<std::size_t, 3>{1, 2, 0},
        std::array<std::size_t, 3>{2, 0, 1},
        std::array<std::size_t, 3>{2, 1, 0},
    };

    for (auto const& order : permutations) {
        SCOPED_TRACE(
            std::to_string(order[0]) + std::to_string(order[1])
            + std::to_string(order[2]));
        iv::ConfiguredGraph graph;
        auto const first = append(graph, PaceRelation{1, 2, 8});
        auto const second = append(graph, PaceRelation{4, 1});
        auto const third = append(graph, PaceRelation{1, 2});
        std::array const connections{
            mono_connection(first, 0, second, 0),
            mono_connection(second, 0, third, 0),
            mono_connection(third, 0, first, 0),
        };
        for (auto const connection : order) {
            graph.connections.record_configured_sample_connection(
                connections[connection]);
        }

        auto realized = iv::graph_jit::resolve_port_realization(graph);
        if (!realized) FAIL() << realized.error();

        EXPECT_EQ(realized->input(first, 0).absolute_pace, iv::PortPace(8));
        EXPECT_EQ(realized->execution_rate(first), iv::PortPace(8));
        EXPECT_EQ(realized->execution_rate(second), iv::PortPace(4));
        EXPECT_EQ(realized->execution_rate(third), iv::PortPace(4));
        EXPECT_EQ(
            realized->output(first, 0).absolute_pace,
            realized->input(second, 0).absolute_pace);
        EXPECT_EQ(
            realized->output(second, 0).absolute_pace,
            realized->input(third, 0).absolute_pace);
        EXPECT_EQ(
            realized->output(third, 0).absolute_pace,
            realized->input(first, 0).absolute_pace);
    }
}

TEST(PortRealization, SortedCoverageRequirementCrossesMixedFanInAndRelay)
{
    constexpr std::array scenarios{
        std::pair{false, false},
        std::pair{false, true},
        std::pair{true, false},
        std::pair{true, true},
    };
    for (auto const [reverse_sources, downstream_first] : scenarios) {
        SCOPED_TRACE(
            std::string(reverse_sources ? "reverse sources, " : "forward sources, ")
            + (downstream_first ? "downstream first" : "upstream first"));
        iv::ConfiguredGraph graph;
        auto const first = append(graph, MixedCoverageOrderingSource{});
        auto const second = append(graph, MixedCoverageOrderingSource{});
        auto const isolated = append(graph, MixedCoverageOrderingSource{});
        auto const relay = append(graph, MixedCoverageOrderingRelay{});
        auto const sink = append(graph, MixedSortedCoverageSink{});

        iv::ConfiguredSampleConnection sample_fan_in{
            .source_type = iv::ChannelTypeId::stereo,
            .source_channels = reverse_sources
                ? std::vector<iv::SampleOutputChannelId>{
                    {second, 0, 0}, {first, 0, 0}}
                : std::vector<iv::SampleOutputChannelId>{
                    {first, 0, 0}, {second, 0, 0}},
            .target_type = iv::ChannelTypeId::mono,
            .target_channels = {{relay, 0, 0}},
        };
        auto const sample_to_sink = mono_connection(relay, 0, sink, 0);
        if (downstream_first) {
            graph.connections.record_configured_sample_connection(
                sample_to_sink);
        }
        graph.connections.record_configured_sample_connection(
            std::move(sample_fan_in));
        if (!downstream_first) {
            graph.connections.record_configured_sample_connection(
                sample_to_sink);
        }

        iv::ConfiguredEventConnection event_fan_in{
            .source_type = iv::EventTypeId::trigger,
            .sources = reverse_sources
                ? std::vector<iv::EventOutputPortId>{
                    {second, 0}, {first, 0}}
                : std::vector<iv::EventOutputPortId>{
                    {first, 0}, {second, 0}},
            .target_type = iv::EventTypeId::trigger,
            .targets = {{relay, 0}},
        };
        iv::ConfiguredEventConnection const event_to_sink{
            .source_type = iv::EventTypeId::trigger,
            .sources = {{relay, 0}},
            .target_type = iv::EventTypeId::trigger,
            .targets = {{sink, 0}},
        };
        if (downstream_first) {
            graph.connections.record_configured_event_connection(
                event_to_sink);
        }
        graph.connections.record_configured_event_connection(
            std::move(event_fan_in));
        if (!downstream_first) {
            graph.connections.record_configured_event_connection(
                event_to_sink);
        }

        auto realized = iv::graph_jit::resolve_port_realization(graph);
        if (!realized) FAIL() << realized.error();

        for (auto const source : {first, second}) {
            EXPECT_EQ(
                realized->output(source, 0).ordering,
                iv::CoverageOrdering::sorted);
            EXPECT_EQ(
                realized->event_output(source, 0).ordering,
                iv::CoverageOrdering::sorted);
        }
        EXPECT_EQ(
            realized->input(relay, 0).ordering,
            iv::CoverageOrdering::sorted);
        EXPECT_EQ(
            realized->output(relay, 0).ordering,
            iv::CoverageOrdering::sorted);
        EXPECT_EQ(
            realized->input(sink, 0).ordering,
            iv::CoverageOrdering::sorted);
        EXPECT_EQ(
            realized->event_input(relay, 0).ordering,
            iv::CoverageOrdering::sorted);
        EXPECT_EQ(
            realized->event_output(relay, 0).ordering,
            iv::CoverageOrdering::sorted);
        EXPECT_EQ(
            realized->event_input(sink, 0).ordering,
            iv::CoverageOrdering::sorted);
        EXPECT_EQ(
            realized->output(isolated, 0).ordering,
            iv::CoverageOrdering::unspecified);
        EXPECT_EQ(
            realized->event_output(isolated, 0).ordering,
            iv::CoverageOrdering::unspecified);
    }
}

} // namespace
