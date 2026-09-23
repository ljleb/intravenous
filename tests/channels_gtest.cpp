#include <intravenous/basic_nodes/constant.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/host.hpp>

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace {
using iv::operator""_P;

static_assert(iv::channel_count(iv::ChannelTypeId::mono) == 1);
static_assert(iv::channel_count(iv::ChannelTypeId::stereo) == 2);
static_assert(iv::stereo::left.channel_ordinal == 0);
static_assert(iv::stereo::right.channel_ordinal == 1);
static_assert(iv::details::has_constexpr_port_configs<
    iv::ChannelPack<iv::stereo>>);
static_assert(iv::details::has_constexpr_port_configs<
    iv::ChannelUnpack<iv::stereo>>);

struct NamedStereoSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::realtime_sample_output(
            "main", {
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
        return std::array<iv::InputConfig, 1>{
            iv::realtime_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{
            iv::realtime_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<MonoPass> const& ctx) const
    {
        ctx.outputs[0].push_block(ctx.inputs[0].get_block(ctx.block_size));
    }
};

TEST(Channels, SamplePortStorageViewConstructsInvocationLocalFacades)
{
    std::array<iv::Sample, 8> samples{};
    iv::SamplePortStorageView storage{
        std::span<iv::Sample>{samples},
        0,
        iv::mono_planar_channel_layout,
        samples.size(),
    };
    iv::OutputPort output(storage, 0, 6);
    iv::InputPort input(storage, 0, 0, 6);

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
    // This is an input projection: each channel has its own physical ring
    // capacity and read delay. OutputPort writes producer storage without
    // applying an input's per-channel frame_delay.
    iv::InputPort input(storage, 0, 0, 10);

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
    iv::InputPort input(storage, 0, 0, 5);

    EXPECT_FLOAT_EQ(input.get_frame(0, 0), 105.0f);
    EXPECT_FLOAT_EQ(input.get_frame(0, 1), 205.0f);
    EXPECT_FLOAT_EQ(input.get_frame(2, 0), 107.0f);
    EXPECT_FLOAT_EQ(input.get_frame(2, 1), 207.0f);
}

TEST(Channels, ChannelPackAndUnpackRetainStaticPortSchemas)
{
    auto const pack = iv::ChannelPack<iv::stereo>::inputs();
    auto const packed = iv::ChannelPack<iv::stereo>::outputs();
    auto const unpack = iv::ChannelUnpack<iv::stereo>::inputs();
    auto const unpacked = iv::ChannelUnpack<iv::stereo>::outputs();

    EXPECT_EQ(pack.size(), 2u);
    EXPECT_EQ(packed.size(), 1u);
    EXPECT_EQ(unpack.size(), 1u);
    EXPECT_EQ(unpacked.size(), 2u);
    EXPECT_EQ(iv::sample_properties(packed.front()).channel_layout.channel_type,
              iv::ChannelTypeId::stereo);
    EXPECT_EQ(iv::sample_properties(unpack.front()).channel_layout.channel_type,
              iv::ChannelTypeId::stereo);
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
    auto const mono = graph.input<"mono">(iv::Sample{0.25f});
    auto const stereo = graph.input<"stereo", iv::stereo>(iv::Sample{0.5f});
    auto const families = iv::host::public_sample_input_families(graph);

    EXPECT_EQ(static_cast<iv::SamplePortRef>(mono).channel_type,
              iv::ChannelTypeId::mono);
    EXPECT_EQ(static_cast<iv::SamplePortRef>(stereo).channel_type,
              iv::ChannelTypeId::stereo);
    ASSERT_EQ(families.families.size(), 2u);
    EXPECT_EQ(families.families[0].channels.size(), 1u);
    EXPECT_EQ(families.families[1].channels.size(), 2u);
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
