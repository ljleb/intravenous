#include <intravenous/channel_layout.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/node/tick.h>
#include <intravenous/graph/builder.h>
#include <intravenous/dsl.h>
#include <intravenous/runtime/sample_stream_blocks.h>

#include <gtest/gtest.h>

namespace {

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
        ctx.outputs[0].push_frame(std::array<iv::Sample, 2>{0.0f, 0.0f});
    }
};

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

TEST(Channels, ChannelBoundaryAdaptersInsertAsOrdinaryGraphNodes)
{
    iv::GraphBuilder g;
    (void)g.node<iv::ChannelPack<iv::stereo>>();
    (void)g.node<iv::ChannelUnpack<iv::stereo>>();
    g.outputs();

    auto const built = g.build_root_node();
    EXPECT_EQ(built.graph.outputs().size(), 0u);
}

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
