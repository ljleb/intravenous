#include <intravenous/basic_nodes/constant.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/host.hpp>
#include <intravenous/graph/node_ports.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace {
using iv::operator""_P;

iv::GlobalIndex index(std::int64_t value)
{
    return iv::GlobalIndex::from_signed(value);
}

static_assert(iv::channel_count(iv::ChannelTypeId::mono) == 1);
static_assert(iv::channel_count(iv::ChannelTypeId::stereo) == 2);
static_assert(iv::stereo::left.channel_index == 0);
static_assert(iv::stereo::right.channel_index == 1);
static_assert(iv::channel_type_info(iv::ChannelTypeId::mono).wire_name == "mono");
static_assert(iv::channel_type_info(iv::ChannelTypeId::stereo).channel_count == 2);
static_assert(iv::channel_conversion_coefficient(
    iv::ChannelTypeId::mono, iv::ChannelTypeId::stereo, 0, 1) == 1.0f);
static_assert(iv::channel_conversion_coefficient(
    iv::ChannelTypeId::stereo, iv::ChannelTypeId::mono, 0, 0) == 0.5f);
static_assert(iv::channel_conversion_coefficient(
    iv::ChannelTypeId::stereo, iv::ChannelTypeId::mono, 1, 0) == 0.5f);

struct NamedStereoSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("main", iv::OutputProduction::tick, {
                .channel_layout = {
                    .channel_type = iv::ChannelTypeId::stereo,
                    .sample_layout = iv::SampleStreamLayout::planar,
                },
            })};
    }

    void tick_block(iv::TickBlockContext<NamedStereoSource> const&) const {}
};

struct MonoPass {
    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void tick_block(iv::TickBlockContext<MonoPass> const& ctx) const
    {
        ctx.template output<0>().push_block(ctx.template input<0>().block());
    }
};

TEST(Channels, RegistryDerivedConversionPlansApplyDeclaredMatrices)
{
    auto const interleave = iv::ChannelConversionRegistry::plan(
        {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        });
    std::array<iv::Sample, 6> planar_stereo {
        1.0f, 2.0f, 3.0f, 11.0f, 12.0f, 13.0f};
    std::array<iv::Sample, 6> interleaved_stereo {};
    interleave.convert(
        planar_stereo.data(), interleaved_stereo.data(), 3);
    std::array<float, 6> const expected_interleaved {
        1.0f, 11.0f, 2.0f, 12.0f, 3.0f, 13.0f};
    for (std::size_t sample = 0; sample < interleaved_stereo.size(); ++sample) {
        EXPECT_FLOAT_EQ(
            interleaved_stereo[sample], expected_interleaved[sample]);
    }

    auto const duplicate = iv::ChannelConversionRegistry::plan(
        {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        });
    std::array<iv::Sample, 3> mono {1.0f, 2.0f, 3.0f};
    std::array<iv::Sample, 6> stereo {};
    duplicate.convert(mono.data(), stereo.data(), mono.size());
    std::array<float, 6> const expected_stereo {
        1.0f, 1.0f, 2.0f, 2.0f, 3.0f, 3.0f};
    for (std::size_t sample = 0; sample < stereo.size(); ++sample) {
        EXPECT_FLOAT_EQ(stereo[sample], expected_stereo[sample]);
    }

    auto const average = iv::ChannelConversionRegistry::plan(
        {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        },
        {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        });
    std::array<iv::Sample, 3> averaged {};
    average.convert(stereo.data(), averaged.data(), averaged.size());
    for (std::size_t sample = 0; sample < averaged.size(); ++sample) {
        EXPECT_FLOAT_EQ(averaged[sample], mono[sample]);
    }

    EXPECT_EQ(
        iv::channel_conversion_aliased_source(
            iv::ChannelTypeId::mono, iv::ChannelTypeId::stereo, 1),
        0u);
    EXPECT_FALSE(iv::channel_conversion_aliased_source(
        iv::ChannelTypeId::stereo, iv::ChannelTypeId::mono, 0));
    EXPECT_TRUE(iv::channel_conversion_mixes_sources(
        iv::ChannelTypeId::stereo, iv::ChannelTypeId::mono));
}

TEST(Channels, PositionalInputsUseSeparateSampleAndEventIndexes)
{
    iv::NodePorts ports;
    ports.input_configs = {
        iv::erase_port_config(
            iv::input<iv::Sample>("first", iv::InputAccess::sequential)),
        iv::erase_port_config(iv::event_input(
            "first-event",
            iv::EventTypeId::trigger,
            iv::InputAccess::sequential)),
        iv::erase_port_config(
            iv::input<iv::Sample>("second", iv::InputAccess::sequential)),
        iv::erase_port_config(iv::event_input(
            "second-event",
            iv::EventTypeId::trigger,
            iv::InputAccess::sequential)),
    };

    EXPECT_EQ(ports.input_port_at(7, 0),
        (iv::NodeBundlePortId{7, iv::PortKind::sample, 0}));
    EXPECT_EQ(ports.input_port_at(7, 1),
        (iv::NodeBundlePortId{7, iv::PortKind::event, 0}));
    EXPECT_EQ(ports.input_port_at(7, 2),
        (iv::NodeBundlePortId{7, iv::PortKind::sample, 1}));
    EXPECT_EQ(ports.input_port_at(7, 3),
        (iv::NodeBundlePortId{7, iv::PortKind::event, 1}));
}

TEST(Channels, SamplePortStorageViewConstructsInvocationLocalFacades)
{
    std::array<iv::Sample, 8> samples{};
    iv::SamplePortStorageView storage{
        std::span<iv::Sample>{samples},
        0,
        iv::mono_planar_channel_layout,
        samples.size(),
    };
    iv::OutputPort output(storage, 0, index(6));
    iv::InputPort input(storage, 0, 0, index(6));

    for (std::size_t i = 0; i < 4; ++i) {
        output.push(static_cast<iv::Sample>(i + 1));
    }
    auto const block = input.get_block(4);
    ASSERT_EQ(block.size(), 4u);
    EXPECT_FLOAT_EQ(block[0], 1.0f);
    EXPECT_FLOAT_EQ(block[1], 2.0f);
    EXPECT_FLOAT_EQ(block[2], 3.0f);
    EXPECT_FLOAT_EQ(block[3], 4.0f);
}

TEST(Channels, SamplePortStorageViewSupportsIndependentChannelStorage)
{
    std::array<iv::Sample, 8> left{};
    std::array<iv::Sample, 16> right{};
    std::array<
        iv::SampleChannelStorageView,
        iv::maximum_supported_channel_count> channels{};
    channels[0] = {
        .storage = left.data(),
        .frame_capacity = left.size(),
        .frame_stride = 1,
        .frame_delay = 2,
    };
    channels[1] = {
        .storage = right.data(),
        .frame_capacity = right.size(),
        .frame_stride = 1,
        .frame_delay = 5,
    };
    iv::SamplePortStorageView storage{
        channels,
        0,
        {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        left.size(),
    };
    // This is an input projection: each channel has its own storage ring
    // capacity and read delay. OutputPort writes producer storage without
    // applying an input's per-channel frame_delay.
    iv::InputPort input(storage, 0, 0, index(10));

    // Logical frame 10 resolves to left[(10 - 2) & 7] and
    // right[(10 - 5) & 15], respectively.
    left[0] = 1.25f;
    right[5] = 2.5f;
    EXPECT_FLOAT_EQ(input.get_frame(0, 0), 1.25f);
    EXPECT_FLOAT_EQ(input.get_frame(0, 1), 2.5f);

    // Ring wrap uses each channel's capacity, not the view's capacity.
    left[7] = 3.75f;
    right[15] = 4.5f;
    EXPECT_FLOAT_EQ(input.get_frame(7, 0), 3.75f);
    EXPECT_FLOAT_EQ(input.get_frame(10, 1), 4.5f);
}

TEST(Channels, SamplePortStorageViewSupportsStridedChannelPointers)
{
    std::array<iv::Sample, 16> interleaved{};
    for (std::size_t frame = 0; frame < 8; ++frame) {
        interleaved[frame * 2] = static_cast<iv::Sample>(100 + frame);
        interleaved[frame * 2 + 1] = static_cast<iv::Sample>(200 + frame);
    }
    std::array<
        iv::SampleChannelStorageView,
        iv::maximum_supported_channel_count> channels{};
    channels[0] = {
        .storage = interleaved.data(),
        .frame_capacity = 8,
        .frame_stride = 2,
    };
    channels[1] = {
        .storage = interleaved.data() + 1,
        .frame_capacity = 8,
        .frame_stride = 2,
    };
    iv::SamplePortStorageView storage{
        channels,
        0,
        {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        },
        8,
    };
    iv::InputPort input(storage, 0, 0, index(5));

    EXPECT_FLOAT_EQ(input.get_frame(0, 0), 105.0f);
    EXPECT_FLOAT_EQ(input.get_frame(0, 1), 205.0f);
    EXPECT_FLOAT_EQ(input.get_frame(2, 0), 107.0f);
    EXPECT_FLOAT_EQ(input.get_frame(2, 1), 207.0f);
}

TEST(Channels, SampleRefsExposeOrderedStructuralChannelIdentity)
{
    iv::GraphBuilder graph;
    auto source = iv::details::configure_concrete_node<NamedStereoSource>(graph);
    auto stream = source[iv::PortName<"main">{}];
    auto const erased = static_cast<iv::SamplePortRef>(stream);
    auto const left = static_cast<iv::SamplePortRef>(
        stream[iv::stereo::left]);

    ASSERT_EQ(erased.channels().size(), 2u);
    EXPECT_EQ(erased.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(erased.channels()[0].bundle, source.node_bundle_handle());
    EXPECT_EQ(erased.channels()[0].channel, 0u);
    EXPECT_EQ(erased.channels()[1].channel, 1u);
    ASSERT_EQ(left.channels().size(), 1u);
    EXPECT_EQ(left.channel_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(left.channels().front(), erased.channels().front());
}

TEST(Channels, GraphBuilderTileIsPureStructuralComposition)
{
    iv::GraphBuilder graph;
    auto left = iv::details::configure_concrete_node<iv::Constant>(
        graph, iv::Sample{0.25f});
    auto right = iv::details::configure_concrete_node<iv::Constant>(
        graph, iv::Sample{-0.5f});
    auto const tiled = static_cast<iv::SamplePortRef>(
        graph.tile<iv::stereo>(left, right));

    ASSERT_EQ(tiled.channels().size(), 2u);
    EXPECT_EQ(tiled.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(tiled.channels()[0].bundle, left.node_bundle_handle());
    EXPECT_EQ(tiled.channels()[1].bundle, right.node_bundle_handle());
}

TEST(Channels, TiledDetachStaysConfiguredConnectionMetadata)
{
    iv::GraphBuilder graph;
    auto left = iv::details::configure_concrete_node<iv::Constant>(
        graph, iv::Sample{0.25f});
    auto right = iv::details::configure_concrete_node<iv::Constant>(
        graph, iv::Sample{-0.5f});
    auto detached = static_cast<iv::SamplePortRef>(
        graph.tile<iv::stereo>(left, right)).detach();
    graph.outputs(detached);
    auto configured = std::move(graph).finish();

    auto const& connections =
        configured.connections.configured_sample_connections();
    ASSERT_EQ(connections.size(), 1u);
    EXPECT_EQ(connections.front().source_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(connections.front().source_channels.size(), 2u);
    ASSERT_TRUE(connections.front().detach.has_value());
    EXPECT_EQ(connections.front().detach->loop_extra_latency, 1u);
}

TEST(Channels, NamedPublicInputsRetainRequestedChannelTypes)
{
    iv::GraphBuilder graph;
    auto const default_type = graph.input<"default-type">();
    auto const mono = graph.input<"mono">(iv::Sample{0.25f});
    auto const stereo = graph.input<"stereo", iv::stereo>(iv::Sample{0.5f});
    auto const families = iv::host::public_sample_input_families(graph);

    static_assert(std::same_as<iv::DefaultContinuousValueType, iv::Sample>);
    ASSERT_EQ(families.families.size(), 3u);
    EXPECT_EQ(iv::continuous_value_type_domain(families.families[0].input_config),
              iv::ContinuousValueTypeSet::of<iv::DefaultContinuousValueType>());
    EXPECT_EQ(static_cast<iv::SamplePortRef>(default_type).channel_type,
              iv::ChannelTypeId::mono);
    EXPECT_EQ(static_cast<iv::SamplePortRef>(mono).channel_type,
              iv::ChannelTypeId::mono);
    EXPECT_EQ(static_cast<iv::SamplePortRef>(stereo).channel_type,
              iv::ChannelTypeId::stereo);
    EXPECT_EQ(families.families[0].channels.size(), 1u);
    EXPECT_EQ(families.families[1].channels.size(), 1u);
    EXPECT_EQ(families.families[2].channels.size(), 2u);
}

TEST(Channels, PublicInputsRetainDeclaredContinuousValueDomains)
{
    auto const coordinate_default = iv::FractionalGlobalIndex::from_parts(
        iv::GlobalIndex::from_signed(-2), std::uint64_t{17});

    iv::GraphBuilder graph;
    auto const coordinate =
        graph.input<"coordinate", iv::FractionalGlobalIndex>({
            .default_value = coordinate_default,
        });
    auto const spectrum = graph.input<"spectrum", iv::FFTBlock>({
        .channel_layout = {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
    });
    auto const either =
        graph.input<"either", iv::Sample, iv::FractionalGlobalIndex>(
            {.default_value = iv::Sample{0.25f}},
            {.default_value = coordinate_default});

    auto const families = iv::host::public_sample_input_families(graph);
    ASSERT_EQ(families.families.size(), 3u);

    auto const& coordinate_config = families.families[0].input_config;
    EXPECT_EQ(coordinate_config.name, "coordinate");
    EXPECT_EQ(iv::continuous_value_type_domain(coordinate_config),
              iv::ContinuousValueTypeSet::of<iv::FractionalGlobalIndex>());
    EXPECT_EQ(iv::sequential_input_metadata<iv::FractionalGlobalIndex>(
                  iv::sequential_input_properties(coordinate_config))
                  .default_value,
              coordinate_default);

    auto const& spectrum_config = families.families[1].input_config;
    EXPECT_EQ(spectrum_config.name, "spectrum");
    EXPECT_EQ(iv::continuous_value_type_domain(spectrum_config),
              iv::ContinuousValueTypeSet::of<iv::FFTBlock>());
    EXPECT_EQ(spectrum_config.channel_layout.channel_type,
              iv::ChannelTypeId::stereo);

    auto const& either_config = families.families[2].input_config;
    EXPECT_EQ(iv::continuous_value_type_domain(either_config),
              (iv::ContinuousValueTypeSet::of<
                  iv::Sample, iv::FractionalGlobalIndex>()));

    EXPECT_EQ(static_cast<iv::SamplePortRef>(coordinate).channel_type,
              iv::ChannelTypeId::mono);
    EXPECT_EQ(static_cast<iv::SamplePortRef>(spectrum).channel_type,
              iv::ChannelTypeId::stereo);
    EXPECT_EQ(static_cast<iv::SamplePortRef>(either).channel_type,
              iv::ChannelTypeId::mono);
}

TEST(Channels, SubgraphInputsRetainDeclaredContinuousValueDomains)
{
    iv::GraphBuilder graph;
    auto nested = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto default_type = boundary.input<"default-type">();
        auto coordinate =
            boundary.input<"coordinate", iv::FractionalGlobalIndex>();
        (void)default_type;
        (void)coordinate;
    }, "typed-boundary");
    auto const nested_handle = nested.node_bundle_handle();

    auto configured = std::move(graph).finish();
    auto const boundary = configured.node_bundles.bundle(nested_handle)
        .subgraph_boundary_handle();
    ASSERT_TRUE(boundary.has_value());
    auto const inputs = configured.node_bundles.bundle(*boundary)
        .boundary_sample_inputs();
    ASSERT_EQ(inputs.size(), 2u);
    EXPECT_EQ(inputs[0].name, "default-type");
    EXPECT_EQ(iv::continuous_value_type_domain(inputs[0]),
              iv::ContinuousValueTypeSet::of<iv::DefaultContinuousValueType>());
    EXPECT_EQ(inputs[1].name, "coordinate");
    EXPECT_EQ(iv::continuous_value_type_domain(inputs[1]),
              iv::ContinuousValueTypeSet::of<iv::FractionalGlobalIndex>());
}

TEST(Channels, ErasedRefsRuntimeCheckChannelMembers)
{
    iv::GraphBuilder graph;
    iv::NodeRef stereo =
        iv::details::configure_concrete_node<NamedStereoSource>(graph).node_ref();
    iv::NodeRef mono =
        iv::details::configure_concrete_node<MonoPass>(graph).node_ref();

    EXPECT_EQ(static_cast<iv::SamplePortRef>(
                  stereo["main"][iv::stereo::left]).channel_type,
              iv::ChannelTypeId::mono);
    EXPECT_THROW((void)stereo["main"][iv::mono::center], std::logic_error);
    EXPECT_THROW((void)mono[iv::stereo::left], std::logic_error);
}

} // namespace
