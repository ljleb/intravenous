#include <intravenous/channel_layout.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/node/tick.h>
#include <intravenous/node/block_executor.h>
#include <intravenous/graph/builder.h>
#include <intravenous/dsl.h>
#include <intravenous/runtime/sample_stream_blocks.h>

#include <gtest/gtest.h>

namespace {
using iv::operator""_F;

static_assert(iv::channel_count(iv::ChannelTypeId::mono) == 1);
static_assert(iv::channel_count(iv::ChannelTypeId::stereo) == 2);
static_assert(iv::mono::channel_count == 1);
static_assert(iv::stereo::channel_count == 2);
static_assert(iv::stereo::left.channel_ordinal == 0);
static_assert(iv::stereo::right.channel_ordinal == 1);
static_assert(iv::ChannelTypeTraits<iv::stereo>::id == iv::ChannelTypeId::stereo);
static_assert(iv::details::has_constexpr_sample_port_configs<iv::ChannelPack<iv::stereo>>);
static_assert(iv::details::has_constexpr_sample_port_configs<iv::ChannelUnpack<iv::stereo>>);
static_assert(iv::sample_storage_size(
    iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    },
    7) == 14);

iv::BorrowedSampleBlock borrowed_block(
    std::span<iv::Sample const> samples,
    iv::ChannelLayout layout,
    size_t frame_count)
{
    return iv::BorrowedSampleBlock{
        .samples = samples,
        .channel_layout = layout,
        .frame_count = frame_count,
    };
}

struct PlanarStereoCopy {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{iv::InputConfig{
            .name = "audio",
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        }};
    }
    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::OutputConfig{
            .name = "main",
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        }};
    }
    void tick_block(iv::TickBlockContext<PlanarStereoCopy> const& ctx) const
    {
        auto input = ctx.template input<"audio">();
        auto output = ctx.template output<"main">();
        for (size_t frame = 0; frame < ctx.block_size; ++frame) {
            output[iv::stereo::left][frame] = input[iv::stereo::left][frame];
            output[iv::stereo::right][frame] = input[iv::stereo::right][frame];
        }
    }
};

struct InterleavedStereoCopy {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{iv::InputConfig{
            .name = "audio",
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
        }};
    }
    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::OutputConfig{
            .name = "main",
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
        }};
    }
    void tick_block(iv::TickBlockContext<InterleavedStereoCopy> const& ctx) const
    {
        auto input = ctx.template input<"audio">();
        auto output = ctx.template output<"main">();
        for (size_t frame = 0; frame < ctx.block_size; ++frame) {
            output[frame][iv::stereo::left] = input[frame][iv::stereo::left];
            output[frame][iv::stereo::right] = input[frame][iv::stereo::right];
        }
    }
};

struct NamedStereoSource {
    iv::Sample left = 0.0f;
    iv::Sample right = 0.0f;

    NamedStereoSource() = default;
    NamedStereoSource(iv::Sample left_, iv::Sample right_) : left(left_), right(right_) {}
    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::OutputConfig{
            .name = "main",
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        }};
    }

    void tick(iv::TickSampleContext<NamedStereoSource> const& ctx) const
    {
        ctx.outputs[0].push_frame(std::array<iv::Sample, 2>{left, right});
    }
};

struct StereoBufferSink {
    iv::Sample* left = nullptr;
    iv::Sample* right = nullptr;

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{iv::InputConfig{
            .name = "in",
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        }};
    }

    void tick(iv::TickSampleContext<StereoBufferSink> const& ctx) const
    {
        *left = ctx.inputs[0].get(0, 0);
        *right = ctx.inputs[0].get(0, 1);
    }
};

struct NonConstexprPorts {
    static auto inputs() { return std::array<iv::InputConfig, 1>{}; }
    static auto outputs() { return std::array<iv::OutputConfig, 1>{}; }
};

bool has_generated_type(std::vector<std::string> const& types, std::string_view name)
{
    return std::ranges::any_of(types, [name](std::string const& type) {
        return type.contains(name);
    });
}

struct NamedInterleavedStereoSource {
    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::OutputConfig{
            .name = "main",
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
        }};
    }

    void tick(iv::TickSampleContext<NamedInterleavedStereoSource> const& ctx) const
    {
        ctx.outputs[0].push_frame(std::array<iv::Sample, 2>{0.0f, 0.0f});
    }
};

struct MonoPass {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{iv::InputConfig{.name = "in"}};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::OutputConfig{.name = "out"}};
    }

    void tick(iv::TickSampleContext<MonoPass> const& ctx) const
    {
        ctx.outputs[0].push(ctx.inputs[0].get());
    }
};

struct EventfulMonoPass {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{iv::InputConfig{.name = "in"}};
    }
    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::OutputConfig{.name = "out"}};
    }
    static constexpr auto event_inputs()
    {
        return std::array<iv::EventInputConfig, 1>{iv::EventInputConfig{
            .name = "trigger", .type = iv::EventTypeId::trigger}};
    }
    static constexpr auto event_outputs()
    {
        return std::array<iv::EventOutputConfig, 1>{iv::EventOutputConfig{
            .name = "trigger", .type = iv::EventTypeId::trigger}};
    }
    void tick(iv::TickSampleContext<EventfulMonoPass> const& ctx) const
    {
        ctx.outputs[0].push(ctx.inputs[0].get());
    }
};

struct MonoBufferSink {
    iv::Sample* destination = nullptr;

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{};
    }

    void tick(iv::TickSampleContext<MonoBufferSink> const& ctx) const
    {
        *destination = ctx.inputs[0].get();
    }
};

std::vector<iv::Sample> sample_values(iv::SampleStorageBlock const& block)
{
    std::vector<iv::Sample> values;
    values.reserve(block.samples.size());
    for (auto const sample : block.samples) {
        values.push_back(iv::Sample{sample});
    }
    return values;
}

}

TEST(Channels, MonoPlanarIdentityConversionPreservesExactSamples)
{
    auto const samples = std::array<iv::Sample, 5>{
        iv::Sample{1.0f},
        iv::Sample{-2.0f},
        iv::Sample{3.5f},
        iv::Sample{0.25f},
        iv::Sample{-0.75f},
    };

    auto const converted = iv::copy_sample_storage_block(
        borrowed_block(
            samples,
            iv::ChannelLayout{
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            samples.size())
            .view(),
        iv::ChannelLayout{
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        });

    EXPECT_EQ(converted.channel_layout.channel_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(converted.channel_layout.sample_layout, iv::SampleStreamLayout::planar);
    EXPECT_EQ(converted.frame_count, samples.size());
    EXPECT_EQ(sample_values(converted), (std::vector<iv::Sample>{
        iv::Sample{1.0f},
        iv::Sample{-2.0f},
        iv::Sample{3.5f},
        iv::Sample{0.25f},
        iv::Sample{-0.75f},
    }));
}

TEST(Channels, StereoInterleavedIdentityConversionPreservesExactSamples)
{
    auto const samples = std::array<iv::Sample, 8>{
        iv::Sample{1.0f},  iv::Sample{10.0f},
        iv::Sample{2.0f},  iv::Sample{20.0f},
        iv::Sample{3.0f},  iv::Sample{30.0f},
        iv::Sample{4.0f},  iv::Sample{40.0f},
    };

    auto const converted = iv::copy_sample_storage_block(
        borrowed_block(
            samples,
            iv::ChannelLayout{
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
            4)
            .view(),
        iv::ChannelLayout{
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        });

    EXPECT_EQ(converted.channel_layout.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(converted.channel_layout.sample_layout, iv::SampleStreamLayout::interleaved);
    EXPECT_EQ(converted.frame_count, 4u);
    EXPECT_EQ(sample_values(converted), (std::vector<iv::Sample>{
        iv::Sample{1.0f},  iv::Sample{10.0f},
        iv::Sample{2.0f},  iv::Sample{20.0f},
        iv::Sample{3.0f},  iv::Sample{30.0f},
        iv::Sample{4.0f},  iv::Sample{40.0f},
    }));
}

TEST(Channels, ZeroFrameConversionProducesEmptyStorage)
{
    auto const converted = iv::copy_sample_storage_block(
        borrowed_block(
            std::span<iv::Sample const>{},
            iv::ChannelLayout{
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            0)
            .view(),
        iv::ChannelLayout{
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        });

    EXPECT_TRUE(converted.samples.empty());
    EXPECT_EQ(converted.frame_count, 0u);
    EXPECT_EQ(converted.channel_layout.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(converted.channel_layout.sample_layout, iv::SampleStreamLayout::interleaved);
}

template<class Node>
void expect_static_stereo_copy(iv::ChannelLayout layout)
{
    std::array<iv::Sample, 16> input_samples{};
    std::array<iv::Sample, 16> output_samples{};
    iv::SharedPortData input_data(input_samples, 0, layout, 8);
    iv::SharedPortData output_data(output_samples, 0, layout, 8);
    iv::InputPort input(input_data, 0);
    iv::OutputPort output(output_data, 0);
    std::array<iv::InputPort, 1> inputs{input};
    std::array<iv::OutputPort, 1> outputs{output};

    for (size_t frame = 0; frame < 4; ++frame) {
        input_samples[input_data.sample_index(frame, 0)] = iv::Sample{static_cast<float>(frame + 1)};
        input_samples[input_data.sample_index(frame, 1)] = iv::Sample{static_cast<float>(frame + 11)};
    }
    iv::do_tick_block(Node{}, iv::TickBlockContext<Node>{
        iv::TickContext<Node>{
            .inputs = inputs,
            .outputs = outputs,
            .event_inputs = {},
            .event_outputs = {},
            .scc_feedback_latency = 0,
            .buffer = {},
        }, 0, 4});

    for (size_t frame = 0; frame < 4; ++frame) {
        EXPECT_EQ(output_samples[output_data.sample_index(frame, 0)], iv::Sample{static_cast<float>(frame + 1)});
        EXPECT_EQ(output_samples[output_data.sample_index(frame, 1)], iv::Sample{static_cast<float>(frame + 11)});
    }
}

TEST(Channels, StaticPlanarContextAccessIsChannelThenSample)
{
    expect_static_stereo_copy<PlanarStereoCopy>({
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::planar,
    });
}

TEST(Channels, StaticInterleavedContextAccessIsSampleThenChannel)
{
    expect_static_stereo_copy<InterleavedStereoCopy>({
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    });
}

TEST(Channels, ChannelUnpackProjectsEachPlanarStereoChannelToMonoOutput)
{
    constexpr auto stereo_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    constexpr auto mono_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    std::array<iv::Sample, 16> input_samples{};
    std::array<iv::Sample, 8> left_samples{};
    std::array<iv::Sample, 8> right_samples{};
    iv::SharedPortData input_data(input_samples, 0, stereo_layout, 8);
    iv::SharedPortData left_data(left_samples, 0, mono_layout, 8);
    iv::SharedPortData right_data(right_samples, 0, mono_layout, 8);
    input_samples[input_data.sample_index(0, 0)] = iv::Sample{2.0f};
    input_samples[input_data.sample_index(0, 1)] = iv::Sample{-3.0f};

    std::array<iv::InputPort, 1> inputs{iv::InputPort(input_data, 0)};
    std::array<iv::OutputPort, 2> outputs{
        iv::OutputPort(left_data, 0),
        iv::OutputPort(right_data, 0),
    };
    using Node = iv::ChannelUnpack<iv::stereo>;
    iv::do_tick(Node{}, iv::TickSampleContext<Node>{
        iv::TickContext<Node>{
            .inputs = inputs,
            .outputs = outputs,
            .event_inputs = {},
            .event_outputs = {},
            .scc_feedback_latency = 0,
            .buffer = {},
        },
        0,
    });

    EXPECT_EQ(left_samples[left_data.sample_index(0, 0)], iv::Sample{2.0f});
    EXPECT_EQ(right_samples[right_data.sample_index(0, 0)], iv::Sample{-3.0f});
}

TEST(Channels, ChannelPackCombinesIndependentMonoInputsIntoPlanarStereoOutput)
{
    constexpr auto mono_layout = iv::ChannelLayout{.channel_type = iv::ChannelTypeId::mono,
                                                     .sample_layout = iv::SampleStreamLayout::planar};
    constexpr auto stereo_layout = iv::ChannelLayout{.channel_type = iv::ChannelTypeId::stereo,
                                                       .sample_layout = iv::SampleStreamLayout::planar};
    std::array<iv::Sample, 4> left_samples{iv::Sample{2.0f}};
    std::array<iv::Sample, 4> right_samples{iv::Sample{-3.0f}};
    std::array<iv::Sample, 8> output_samples{};
    iv::SharedPortData left_data(left_samples, 0, mono_layout, 4);
    iv::SharedPortData right_data(right_samples, 0, mono_layout, 4);
    iv::SharedPortData output_data(output_samples, 0, stereo_layout, 4);
    using Node = iv::ChannelPack<iv::stereo>;
    std::array<iv::InputPort, 2> inputs{iv::InputPort(left_data, 0), iv::InputPort(right_data, 0)};
    std::array<iv::OutputPort, 1> outputs{iv::OutputPort(output_data, 0)};
    iv::do_tick_block(Node{}, iv::TickBlockContext<Node>{iv::TickContext<Node>{
        .inputs = inputs, .outputs = outputs, .event_inputs = {}, .event_outputs = {},
        .scc_feedback_latency = 0, .buffer = {}}, 0, 1});
    EXPECT_EQ(output_samples[output_data.sample_index(0, 0)], iv::Sample{2.0f});
    EXPECT_EQ(output_samples[output_data.sample_index(0, 1)], iv::Sample{-3.0f});
}

TEST(Channels, ChannelBoundaryAdaptersInsertAsOrdinaryGraphNodes)
{
    iv::GraphBuilder g;
    (void)g.node<iv::ChannelPack<iv::stereo>>();
    (void)g.node<iv::ChannelUnpack<iv::stereo>>();
    g.outputs();

    auto const built = g.build_root_node();
    EXPECT_EQ(built.graph.outputs().size(), 0u);
}

TEST(Channels, FullyMonoNodeTilesIntoAStaticStereoOutput)
{
    iv::GraphBuilder g;
    auto source = g.node<iv::Constant, iv::stereo>(iv::Sample{0.25f});
    auto stream = source.get<0>();
    auto left = stream[iv::stereo::left];

    static_assert(std::same_as<decltype(stream),
        iv::TypedSamplePortTileRef<iv::stereo>>);
    static_assert(std::same_as<decltype(left),
        iv::TypedSamplePortTileChannelRef<
            iv::stereo,
            std::remove_cvref_t<decltype(iv::stereo::left)>>>);

    g.outputs(iv::PortName<"left">{} = left);
    auto const built = g.build_root_node();
    ASSERT_EQ(built.graph.outputs().size(), 1u);
    EXPECT_EQ(built.graph.outputs().front().channel_layout.channel_type,
              iv::ChannelTypeId::mono);
}

TEST(Channels, SampleRefsExposeOrderedStructuralChannelIdentity)
{
    iv::GraphBuilder g;
    auto source = g.node<NamedStereoSource>(
        iv::Sample{0.25f}, iv::Sample{-0.5f});
    auto stream = source[iv::PortName<"main">{}];
    auto erased = static_cast<iv::SamplePortRef>(stream);

    EXPECT_EQ(erased.channel_type, iv::ChannelTypeId::stereo);
    ASSERT_EQ(erased.channels.size(), 2u);
    EXPECT_EQ(erased.channels[0].bundle, source.node_bundle_handle());
    EXPECT_EQ(erased.channels[0].port, 0u);
    EXPECT_EQ(erased.channels[0].channel, 0u);
    EXPECT_EQ(erased.channels[1].bundle, source.node_bundle_handle());
    EXPECT_EQ(erased.channels[1].port, 0u);
    EXPECT_EQ(erased.channels[1].channel, 1u);

    auto left = static_cast<iv::SamplePortRef>(
        stream[iv::stereo::left]);
    EXPECT_EQ(left.channel_type, iv::ChannelTypeId::mono);
    ASSERT_EQ(left.channels.size(), 1u);
    EXPECT_EQ(left.channels.front(), erased.channels.front());

    g.outputs();
    auto const built = g.build_root_node();
    EXPECT_FALSE(has_generated_type(
        built.metadata.concrete_node_type_identities, "ChannelUnpack"));
}

TEST(Channels, GraphBuilderTileIsPureStructuralComposition)
{
    iv::GraphBuilder g;
    auto left = g.node<iv::Constant>(iv::Sample{0.25f});
    auto right = g.node<iv::Constant>(iv::Sample{-0.5f});
    auto tiled = g.tile<iv::stereo>(left, right);
    auto erased = static_cast<iv::SamplePortRef>(tiled);

    EXPECT_EQ(erased.channel_type, iv::ChannelTypeId::stereo);
    ASSERT_EQ(erased.channels.size(), 2u);
    EXPECT_EQ(erased.channels[0].bundle, left.node_bundle_handle());
    EXPECT_EQ(erased.channels[1].bundle, right.node_bundle_handle());

    g.outputs();
    auto const built = g.build_root_node();
    EXPECT_FALSE(has_generated_type(
        built.metadata.concrete_node_type_identities, "ChannelPack"));
    EXPECT_FALSE(has_generated_type(
        built.metadata.concrete_node_type_identities, "ChannelUnpack"));
}

TEST(Channels, ChannelQualifiedPublicOutputsArePackedOnlyAtCompletion)
{
    iv::GraphBuilder g;
    auto left = g.node<iv::Constant>(iv::Sample{0.25f});
    auto right = g.node<iv::Constant>(iv::Sample{-0.5f});

    g.outputs(
        iv::PortName<"main">{}[iv::stereo::left] =
            static_cast<iv::SamplePortRef>(left),
        iv::PortName<"main">{}[iv::stereo::right] =
            static_cast<iv::SamplePortRef>(right));

    // outputs(...) must not author a ChannelPack. The next bundle is therefore
    // immediately after the two user-authored sources.
    auto after_outputs = g.node<iv::Constant>(iv::Sample{1.0f});
    EXPECT_EQ(
        after_outputs.node_bundle_handle(),
        right.node_bundle_handle() + 1);

    auto const built = g.build_root_node();
    ASSERT_EQ(built.graph.outputs().size(), 1u);
    EXPECT_EQ(
        built.graph.outputs().front().channel_layout.channel_type,
        iv::ChannelTypeId::stereo);
    EXPECT_EQ(
        std::ranges::count_if(
            built.metadata.concrete_node_type_identities,
            [](std::string const& type) {
                return type.contains("ChannelPack");
            }),
        1);
}

TEST(Channels, DetachAuthorsOnlyItsExplicitWriterAndReaderNodes)
{
    iv::GraphBuilder g;
    auto left = g.node<iv::Constant>(iv::Sample{0.25f});
    auto right = g.node<iv::Constant>(iv::Sample{-0.5f});
    auto structural = g.tile<iv::stereo>(left, right);
    auto detached =
        static_cast<iv::SamplePortRef>(structural).detach();

    // Structural stereo still needs a compatibility pack for the current mono
    // detach writer, but that node must be synthesized only in completion.
    auto after_detach = g.node<iv::Constant>(iv::Sample{1.0f});
    EXPECT_EQ(
        after_detach.node_bundle_handle(),
        right.node_bundle_handle() + 3);

    g.outputs(detached);
    auto const built = g.build_root_node();
    EXPECT_EQ(
        std::ranges::count_if(
            built.metadata.concrete_node_type_identities,
            [](std::string const& type) {
                return type.contains("ChannelPack");
            }),
        1);
}

TEST(Channels, StructuralTilePacksOnlyWhenNativeStereoIsMaterialized)
{
    iv::GraphBuilder g;
    auto left_source = g.node<iv::Constant>(iv::Sample{0.25f});
    auto right_source = g.node<iv::Constant>(iv::Sample{-0.5f});
    auto tiled = g.tile<iv::stereo>(left_source, right_source);

    iv::Sample left{};
    iv::Sample right{};
    g.node<StereoBufferSink>(&left, &right)(tiled);
    g.outputs();

    auto built = g.build_root_node();
    EXPECT_TRUE(has_generated_type(
        built.metadata.concrete_node_type_identities, "ChannelPack"));
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(std::move(built.graph)), 1);
    executor.tick_block(0);
    EXPECT_EQ(left, iv::Sample{0.25f});
    EXPECT_EQ(right, iv::Sample{-0.5f});
}

TEST(Channels, SelectedNativeChannelUnpacksOnlyWhenMaterialized)
{
    iv::GraphBuilder g;
    auto source = g.node<NamedStereoSource>(
        iv::Sample{0.25f}, iv::Sample{-0.5f});
    auto stream = source[iv::PortName<"main">{}];

    iv::Sample left{};
    g.node<MonoBufferSink>(&left)(stream[iv::stereo::left]);
    g.outputs();

    auto built = g.build_root_node();
    EXPECT_TRUE(has_generated_type(
        built.metadata.concrete_node_type_identities, "ChannelUnpack"));
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(std::move(built.graph)), 1);
    executor.tick_block(0);
    EXPECT_EQ(left, iv::Sample{0.25f});
}

TEST(Channels, TiledNodesLowerMatchingChannelsToIndependentMonoEdges)
{
    iv::GraphBuilder g;
    auto left_source = g.node<iv::Constant>(iv::Sample{0.25f});
    auto right_source = g.node<iv::Constant>(iv::Sample{-0.5f});
    auto pass = g.node<MonoPass, iv::stereo>();
    pass(g.tile<iv::stereo>(left_source, right_source));

    auto stream = pass[iv::PortName<"out">{}];
    auto const semantic_stream = static_cast<iv::SamplePortRef>(stream);
    ASSERT_EQ(semantic_stream.channels.size(), 2u);
    EXPECT_EQ(semantic_stream.channels[0].bundle, pass.node_bundle_handle());
    EXPECT_EQ(semantic_stream.channels[1].bundle, pass.node_bundle_handle());

    iv::Sample left{};
    iv::Sample right{};
    g.node<MonoBufferSink>(&left)(stream[iv::stereo::left]);
    g.node<MonoBufferSink>(&right)(stream[iv::stereo::right]);
    g.outputs();

    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(g.build_root_node().graph), 1);
    executor.tick_block(0);
    EXPECT_EQ(left, iv::Sample{0.25f});
    EXPECT_EQ(right, iv::Sample{-0.5f});
    auto const built = g.build_root_node();
    EXPECT_FALSE(has_generated_type(built.metadata.concrete_node_type_identities, "ChannelPack"));
    EXPECT_FALSE(has_generated_type(built.metadata.concrete_node_type_identities, "ChannelUnpack"));
}

TEST(Channels, TiledEventPortsBroadcastInputsAndMergeOutputs)
{
    iv::GraphBuilder g;
    auto tiled = iv::_annotate_node_source_info(
        g.node<EventfulMonoPass, iv::stereo>(), "eventful-tiled");
    auto source = g.node<iv::EventConcatenation>(0, iv::EventTypeId::trigger);
    tiled("trigger"_F = source.event_port(0));
    auto merged = tiled.event_port(0);
    ASSERT_EQ(merged.sources.size(), 1u);
    EXPECT_EQ(merged.sources.front().bundle, tiled.node_bundle_handle());
    EXPECT_TRUE(tiled.event_input_is_connected(0));
    auto sink = g.node<iv::EventConcatenation>(1, iv::EventTypeId::trigger);
    sink.connect_event_input(0, merged);
    g.outputs();

    auto const virtual_ports = g.virtual_ports();
    ASSERT_EQ(virtual_ports.event_inputs.size(), 1u);
    ASSERT_EQ(virtual_ports.event_outputs.size(), 1u);
    EXPECT_EQ(virtual_ports.event_inputs.front().node_bundle_ports.size(), 1u);
    EXPECT_EQ(virtual_ports.event_outputs.front().node_bundle_ports.size(), 1u);
    EXPECT_EQ(virtual_ports.event_inputs.front().config.type, iv::EventTypeId::trigger);
    auto const built = g.build_root_node();
    auto const merge_count = std::ranges::count_if(
        built.metadata.concrete_node_type_identities, [](std::string const& type) {
            return type.contains("EventConcatenation");
        });
    EXPECT_EQ(merge_count, 3);
}

TEST(Channels, TiledOutputAutomaticallyPacksForNativeStereoInput)
{
    iv::GraphBuilder g;
    auto left_source = g.node<iv::Constant>(iv::Sample{0.25f});
    auto right_source = g.node<iv::Constant>(iv::Sample{-0.5f});
    auto tiled = g.node<MonoPass, iv::stereo>();
    tiled(g.tile<iv::stereo>(left_source, right_source));
    iv::Sample left{};
    iv::Sample right{};
    g.node<StereoBufferSink>(&left, &right)(tiled[iv::PortName<"out">{}]);
    g.outputs();
    auto built = g.build_root_node();
    EXPECT_TRUE(has_generated_type(built.metadata.concrete_node_type_identities, "ChannelPack"));
    auto executor = iv::BlockNodeExecutor::create(iv::TypeErasedNode(std::move(built.graph)), 1);
    executor.tick_block(0);
    EXPECT_EQ(left, iv::Sample{0.25f});
    EXPECT_EQ(right, iv::Sample{-0.5f});
}

TEST(Channels, NativeStereoOutputAutomaticallyUnpacksForTiledInput)
{
    iv::GraphBuilder g;
    auto source = g.node<NamedStereoSource>(iv::Sample{0.25f}, iv::Sample{-0.5f});
    auto tiled = g.node<MonoPass, iv::stereo>();
    tiled(source[iv::PortName<"main">{}]);
    auto stream = tiled[iv::PortName<"out">{}];
    iv::Sample left{};
    iv::Sample right{};
    g.node<MonoBufferSink>(&left)(stream[iv::stereo::left]);
    g.node<MonoBufferSink>(&right)(stream[iv::stereo::right]);
    g.outputs();
    auto built = g.build_root_node();
    EXPECT_TRUE(has_generated_type(built.metadata.concrete_node_type_identities, "ChannelUnpack"));
    auto executor = iv::BlockNodeExecutor::create(iv::TypeErasedNode(std::move(built.graph)), 1);
    executor.tick_block(0);
    EXPECT_EQ(left, iv::Sample{0.25f});
    EXPECT_EQ(right, iv::Sample{-0.5f});
}

TEST(Channels, MonoInputBroadcastsToEveryTiledInputChannel)
{
    iv::GraphBuilder g;
    auto source = g.node<iv::Constant>(iv::Sample{0.75f});
    auto pass = g.node<MonoPass, iv::stereo>();
    pass(source);

    auto stream = pass[iv::PortName<"out">{}];
    iv::Sample left{};
    iv::Sample right{};
    g.node<MonoBufferSink>(&left)(stream[iv::stereo::left]);
    g.node<MonoBufferSink>(&right)(stream[iv::stereo::right]);
    g.outputs();

    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(g.build_root_node().graph), 1);
    executor.tick_block(0);
    EXPECT_EQ(left, iv::Sample{0.75f});
    EXPECT_EQ(right, iv::Sample{0.75f});
}

TEST(Channels, WholeTiledStreamPacksToOneStereoPublicOutput)
{
    iv::GraphBuilder g;
    auto source = g.node<iv::Constant, iv::stereo>(iv::Sample{0.25f});
    g.outputs(iv::PortName<"main">{} = source.get<0>());

    auto const built = g.build_root_node();
    ASSERT_EQ(built.graph.outputs().size(), 1u);
    EXPECT_EQ(built.graph.outputs().front().channel_layout.channel_type,
              iv::ChannelTypeId::stereo);
    EXPECT_EQ(built.graph.outputs().front().channel_layout.sample_layout,
              iv::SampleStreamLayout::planar);
}

TEST(Channels, SourceAnnotationProjectsATiledBundleAsOneStereoVirtualPort)
{
    iv::GraphBuilder g;
    auto node = iv::_annotate_node_source_info(
        g.node<MonoPass, iv::stereo>(), "tiled-pass");
    (void)node;

    auto const inputs = g.virtual_sample_input_families();
    ASSERT_EQ(inputs.families.size(), 1u);
    auto const& input = inputs.families.front();
    EXPECT_EQ(input.virtual_node_id, "tiled-pass");
    EXPECT_EQ(input.channel_type, iv::ChannelTypeId::stereo);
    ASSERT_EQ(input.channels.size(), 2u);
    ASSERT_EQ(input.channels[0].targets.size(), 1u);
    ASSERT_EQ(input.channels[1].targets.size(), 1u);
    EXPECT_EQ(input.channels[0].targets.front().bundle, node.node_bundle_handle());
    EXPECT_EQ(input.channels[1].targets.front().bundle, node.node_bundle_handle());
    EXPECT_NE(input.channels[0].targets.front(), input.channels[1].targets.front());

    auto const outputs = g.virtual_sample_output_families();
    ASSERT_EQ(outputs.families.size(), 1u);
    auto const& output = outputs.families.front();
    EXPECT_EQ(output.virtual_node_id, "tiled-pass");
    EXPECT_EQ(output.channel_type, iv::ChannelTypeId::stereo);
    ASSERT_EQ(output.channels.size(), 2u);
    ASSERT_EQ(output.channels[0].sources.size(), 1u);
    ASSERT_EQ(output.channels[1].sources.size(), 1u);
    EXPECT_EQ(output.channels[0].sources.front().bundle, node.node_bundle_handle());
    EXPECT_EQ(output.channels[1].sources.front().bundle, node.node_bundle_handle());
    EXPECT_NE(output.channels[0].sources.front(), output.channels[1].sources.front());
}

TEST(Channels, IntrospectionKeepsThePromotedLayoutOfAnAnnotatedTiledBundle)
{
    iv::GraphBuilder g;
    auto node = iv::_annotate_node_source_info(
        g.node<MonoPass, iv::stereo>(), "tiled-introspection",
        "/tmp/tiled-module.cpp", 40, 55);
    (void)node;

    auto const metadata = g.build_metadata();
    ASSERT_EQ(metadata.virtual_nodes.size(), 1u);
    auto const& virtual_node = metadata.virtual_nodes.front();
    ASSERT_EQ(virtual_node.sample_inputs.size(), 1u);
    ASSERT_EQ(virtual_node.sample_outputs.size(), 1u);
    ASSERT_EQ(virtual_node.members.size(), 1u);
    ASSERT_EQ(virtual_node.source_spans.size(), 1u);
    EXPECT_EQ(virtual_node.source_spans.front().file_path, "/tmp/tiled-module.cpp");
    EXPECT_EQ(virtual_node.members.front().ordinal, 0u);
    EXPECT_TRUE(virtual_node.members.front().backing_node_id.starts_with("node-bundle:"));
    ASSERT_EQ(virtual_node.members.front().sample_inputs.size(), 1u);
    ASSERT_EQ(virtual_node.members.front().sample_outputs.size(), 1u);
    EXPECT_EQ(virtual_node.members.front().sample_inputs.front().sample_channel_type,
              iv::ChannelTypeId::stereo);
    EXPECT_EQ(virtual_node.members.front().sample_outputs.front().sample_channel_type,
              iv::ChannelTypeId::stereo);
    EXPECT_EQ(virtual_node.sample_inputs.front().sample_channel_type,
              iv::ChannelTypeId::stereo);
    EXPECT_EQ(virtual_node.sample_outputs.front().sample_channel_type,
              iv::ChannelTypeId::stereo);
    auto const rebuilt_metadata = g.build_metadata();
    ASSERT_EQ(rebuilt_metadata.virtual_nodes.size(), 1u);
    ASSERT_EQ(rebuilt_metadata.virtual_nodes.front().members.size(), 1u);
    ASSERT_EQ(rebuilt_metadata.virtual_nodes.front().source_spans.size(), 1u);
    EXPECT_EQ(rebuilt_metadata.virtual_nodes.front().members.front().backing_node_id,
              virtual_node.members.front().backing_node_id);
}

static_assert(!iv::details::has_constexpr_sample_port_configs<NonConstexprPorts>);

TEST(Channels, NamedConcreteOutputCarriesItsStaticChannelType)
{
    iv::GraphBuilder g;
    auto source = g.node<NamedStereoSource>();
    auto stream = source[iv::PortName<"main">{}];
    auto left = stream[iv::stereo::left];

    static_assert(std::same_as<
        decltype(stream),
        iv::TypedSamplePortRef<iv::stereo, iv::SampleStreamLayout::planar>>);
    static_assert(std::same_as<
        decltype(left),
        iv::TypedSamplePortChannelRef<
            iv::stereo,
            iv::SampleStreamLayout::planar,
            std::remove_cvref_t<decltype(iv::stereo::left)>>>);

    g.outputs(iv::PortName<"left_only">{}[iv::stereo::left] = left);
    auto const built = g.build_root_node();
    ASSERT_EQ(built.graph.outputs().size(), 1u);
    EXPECT_EQ(
        built.graph.outputs().front().channel_layout.channel_type,
        iv::ChannelTypeId::stereo);
}

TEST(Channels, TypedStreamOperatorsPreserveChannelTypeAndChoosePlanarForMixedLayouts)
{
    iv::GraphBuilder g;
    auto planar_source = g.node<NamedStereoSource>();
    auto interleaved_source = g.node<NamedInterleavedStereoSource>();
    auto planar = planar_source[iv::PortName<"main">{}];
    auto interleaved = interleaved_source[iv::PortName<"main">{}];

    auto same_layout_sum = planar + planar;
    auto mixed_layout_sum = planar + interleaved;
    auto scalar_sum = 1.0f + planar;
    auto detached = ~planar;

    static_assert(std::same_as<
        decltype(same_layout_sum),
        iv::TypedSamplePortRef<iv::stereo, iv::SampleStreamLayout::planar>>);
    static_assert(std::same_as<
        decltype(mixed_layout_sum),
        iv::TypedSamplePortRef<iv::stereo, iv::SampleStreamLayout::planar>>);
    static_assert(std::same_as<
        decltype(scalar_sum),
        iv::TypedSamplePortRef<iv::stereo, iv::SampleStreamLayout::planar>>);
    static_assert(std::same_as<
        decltype(detached),
        iv::TypedSamplePortRef<iv::stereo, iv::SampleStreamLayout::planar>>);

    g.outputs(
        iv::PortName<"same">{} = same_layout_sum,
        iv::PortName<"mixed">{} = mixed_layout_sum,
        iv::PortName<"scalar">{} = scalar_sum,
        iv::PortName<"detached">{} = detached);
    auto const built = g.build_root_node();
    EXPECT_EQ(built.graph.outputs().size(), 4u);
}

TEST(Channels, OutputPortAppliesMonoToStereoConversionAtItsWriteBoundary)
{
    constexpr auto mono = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    constexpr auto stereo = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };
    std::array<iv::Sample, 16> samples{};
    iv::SharedPortData data(samples, 0, stereo, 8);
    iv::OutputPort output(data, 0, mono, iv::ChannelConversionRegistry::plan(mono, stereo));

    output.push(iv::Sample{2.0f});
    output.push(iv::Sample{-3.0f});

    EXPECT_EQ(samples[data.sample_index(0, 0)], iv::Sample{2.0f});
    EXPECT_EQ(samples[data.sample_index(0, 1)], iv::Sample{2.0f});
    EXPECT_EQ(samples[data.sample_index(1, 0)], iv::Sample{-3.0f});
    EXPECT_EQ(samples[data.sample_index(1, 1)], iv::Sample{-3.0f});
}

TEST(Channels, OutputPortAppliesStereoToMonoConversionAtItsWriteBoundary)
{
    constexpr auto stereo = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    constexpr auto mono = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    std::array<iv::Sample, 8> samples{};
    iv::SharedPortData data(samples, 0, mono, 8);
    iv::OutputPort output(data, 0, stereo, iv::ChannelConversionRegistry::plan(stereo, mono));
    std::array<iv::Sample, 2> frame{iv::Sample{2.0f}, iv::Sample{6.0f}};

    output.push_frame(frame);

    EXPECT_EQ(samples[data.sample_index(0, 0)], iv::Sample{4.0f});
}

TEST(Channels, OddFrameMonoToStereoInterleavedConversionBroadcastsEveryFrame)
{
    auto const samples = std::array<iv::Sample, 5>{
        iv::Sample{1.0f},
        iv::Sample{-2.0f},
        iv::Sample{3.5f},
        iv::Sample{0.25f},
        iv::Sample{-0.75f},
    };

    auto const converted = iv::copy_sample_storage_block(
        borrowed_block(
            samples,
            iv::ChannelLayout{
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            samples.size())
            .view(),
        iv::ChannelLayout{
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        });

    EXPECT_EQ(converted.frame_count, samples.size());
    EXPECT_EQ(sample_values(converted), (std::vector<iv::Sample>{
        iv::Sample{1.0f}, iv::Sample{1.0f},
        iv::Sample{-2.0f}, iv::Sample{-2.0f},
        iv::Sample{3.5f}, iv::Sample{3.5f},
        iv::Sample{0.25f}, iv::Sample{0.25f},
        iv::Sample{-0.75f}, iv::Sample{-0.75f},
    }));
}
