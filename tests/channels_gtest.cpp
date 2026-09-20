#include <intravenous/basic_nodes/routing.h>
#include <intravenous/channel_layout.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/host.hpp>
#include <configured_graph_test_view.h>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>
#include <intravenous/graph/connection_node.hpp>
#include <intravenous/graph/runtime_bindings.h>
#include <intravenous/node/tick.h>
#include <intravenous/runtime/sample_stream_blocks.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using iv::operator""_F;
using iv::operator""_P;

size_t connection_conversion_call_count = 0;
size_t connection_conversion_frame_count = 0;

void tracked_mono_block_copy(
    iv::Sample const* source,
    iv::Sample* target,
    size_t frames)
{
    ++connection_conversion_call_count;
    connection_conversion_frame_count = frames;
    std::copy_n(source, frames, target);
}

static_assert(iv::channel_count(iv::ChannelTypeId::mono) == 1);
static_assert(iv::channel_count(iv::ChannelTypeId::stereo) == 2);
static_assert(iv::mono::channel_count == 1);
static_assert(iv::stereo::channel_count == 2);
static_assert(iv::stereo::left.channel_ordinal == 0);
static_assert(iv::stereo::right.channel_ordinal == 1);
static_assert(iv::ChannelTypeTraits<iv::stereo>::id == iv::ChannelTypeId::stereo);
static_assert(iv::details::has_constexpr_port_configs<iv::ChannelPack<iv::stereo>>);
static_assert(iv::details::has_constexpr_port_configs<iv::ChannelUnpack<iv::stereo>>);
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

std::vector<iv::Sample> sample_values(iv::SampleStorageBlock const& block)
{
    std::vector<iv::Sample> values;
    values.reserve(block.samples.size());
    for (auto const sample : block.samples)
        values.push_back(iv::Sample{sample});
    return values;
}

struct PlanarStereoCopy {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{iv::realtime_sample_input("audio", {
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        })};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::realtime_sample_output("main", {
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        })};
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
        return std::array<iv::InputConfig, 1>{iv::realtime_sample_input("audio", {
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
        })};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::realtime_sample_output("main", {
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
        })};
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

    constexpr NamedStereoSource() = default;
    constexpr NamedStereoSource(iv::Sample left_, iv::Sample right_)
        : left(left_), right(right_)
    {}

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::realtime_sample_output("main", {
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        })};
    }

    void tick(iv::TickSampleContext<NamedStereoSource> const& ctx) const
    {
        ctx.outputs[0].push_frame(std::array<iv::Sample, 2>{left, right});
    }
};

struct NamedInterleavedStereoSource {
    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::realtime_sample_output("main", {
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
        })};
    }

    void tick(iv::TickSampleContext<NamedInterleavedStereoSource> const& ctx) const
    {
        ctx.outputs[0].push_frame(std::array<iv::Sample, 2>{0.0f, 0.0f});
    }
};

struct MonoPass {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{iv::realtime_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::realtime_sample_output("out")};
    }

    void tick(iv::TickSampleContext<MonoPass> const& ctx) const
    {
        ctx.outputs[0].push(ctx.inputs[0].get());
    }
};

struct DefaultMonoPass {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{iv::realtime_sample_input("in", {
            .default_value = iv::Sample{0.75f},
        })};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::realtime_sample_output("out")};
    }

    void tick(iv::TickSampleContext<DefaultMonoPass> const& ctx) const
    {
        ctx.outputs[0].push(ctx.inputs[0].get());
    }
};

struct EventfulMonoPass {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 2>{
            iv::realtime_sample_input("in"),
            iv::realtime_event_input("trigger", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 2>{
            iv::realtime_sample_output("out"),
            iv::realtime_event_output("trigger", iv::EventTypeId::trigger),
        };
    }

    void tick(iv::TickSampleContext<EventfulMonoPass> const& ctx) const
    {
        ctx.outputs[0].push(ctx.inputs[0].get());
    }
};

struct EventBeforeSamplePass {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 2>{
            iv::realtime_event_input("trigger", iv::EventTypeId::trigger),
            iv::realtime_sample_input("in"),
        };
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::realtime_sample_output("out")};
    }

    void tick(iv::TickSampleContext<EventBeforeSamplePass> const& ctx) const
    {
        ctx.outputs[0].push(ctx.inputs[0].get());
    }
};

struct TickFallbackContractTrace {
    std::size_t calls = 0;
    std::array<iv::SampleIndex, 4> indices{};
    std::array<iv::Sample, 4> current_inputs{};
    std::array<iv::Sample, 4> history_inputs{};
    std::array<iv::Sample, 4> previous_outputs{};
    std::array<std::size_t, 4> output_positions{};
};

struct TickFallbackContractProbe {
    TickFallbackContractTrace* trace = nullptr;

    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input(
            "in", {}, {.history = 1})};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output(
            "out", {}, {.history = 1, .latency = 2})};
    }

    void tick(iv::TickSampleContext<TickFallbackContractProbe> const& ctx) const
    {
        auto const slot = trace->calls++;
        if (slot < trace->indices.size()) {
            trace->indices[slot] = ctx.index;
            trace->current_inputs[slot] = ctx.inputs[0].get();
            trace->history_inputs[slot] = ctx.inputs[0].get(1);
            trace->previous_outputs[slot] = ctx.outputs[0].get();
            trace->output_positions[slot] = ctx.outputs[0].position();
        }
        if (ctx.index == 6) {
            ctx.outputs[0].update(iv::Sample{500.0f});
        }
        ctx.outputs[0].push(static_cast<iv::Sample>(ctx.index));
    }
};

struct TickBlockContractTrace {
    iv::SampleIndex index = 0;
    std::size_t block_size = 0;
    iv::Sample anchored_input = 0.0f;
    std::array<iv::Sample, 4> block_inputs{};
    iv::Sample previous_output = 0.0f;
    std::size_t output_position_before = 0;
    std::size_t output_position_after = 0;
};

struct TickBlockContractProbe {
    TickBlockContractTrace* trace = nullptr;

    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input(
            "in", {}, {.history = 1})};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output(
            "out", {}, {.latency = 2})};
    }

    void tick_block(iv::TickBlockContext<TickBlockContractProbe> const& ctx) const
    {
        trace->index = ctx.index;
        trace->block_size = ctx.block_size;
        trace->anchored_input = ctx.inputs[0].get();
        auto const block = ctx.inputs[0].get_block(ctx.block_size);
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            trace->block_inputs[i] = block[i];
        }

        auto& output = ctx.outputs[0];
        trace->previous_output = output.get();
        trace->output_position_before = output.position();
        output.update(iv::Sample{444.0f});
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(block[i]);
        }
        trace->output_position_after = output.position();
    }
};

struct DirectBlockWriteContractTrace {
    std::size_t output_position_before = 0;
    std::size_t output_position_after_write = 0;
};

struct DirectBlockWriteContractProbe {
    DirectBlockWriteContractTrace* trace = nullptr;

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output("out")};
    }

    void tick_block(
        iv::TickBlockContext<DirectBlockWriteContractProbe> const& ctx) const
    {
        trace->output_position_before = ctx.outputs[0].position();
        auto output = ctx.output<"out">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output[i] = static_cast<iv::Sample>(ctx.index + i);
        }
        trace->output_position_after_write = ctx.outputs[0].position();
    }
};

struct SynthesizedStereoSkipProbe {
    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output("out", {
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
        })};
    }
};

struct ScheduledTriggerSource {
    size_t sample_offset = 0;

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{
            iv::realtime_event_output("trigger", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<ScheduledTriggerSource> const& ctx) const
    {
        ctx.event_outputs[0].push(
            iv::TriggerEvent{}, sample_offset, ctx.index, ctx.block_size);
    }
};

struct NonConstexprPorts {
    static auto inputs() { return std::array<iv::InputConfig, 1>{}; }
    static auto outputs() { return std::array<iv::OutputConfig, 1>{}; }
};

static_assert(!iv::details::has_constexpr_port_configs<NonConstexprPorts>);

template<class Range>
constexpr bool has_generated_type(Range const& types, std::string_view name)
{
    return std::ranges::any_of(types, [name](auto const& type) {
        return std::string_view(type).contains(name);
    });
}

iv::RuntimeGraphPlan compile_graph(
    iv::ConfiguredGraphTestView view,
    bool execution_root = false)
{
    auto configured = iv::thaw_configured_graph_for_test(view);
    auto executable = iv::GraphLowerer::lower(
        std::move(configured), {.execution_root = execution_root});
    return iv::GraphCompiler::compile(std::move(executable));
}

template<class Node>
void expect_static_stereo_copy(iv::ChannelLayout layout)
{
    std::array<iv::Sample, 16> input_samples{};
    std::array<iv::Sample, 16> output_samples{};
    iv::SharedPortData input_data(input_samples, 0, layout, 8);
    iv::SharedPortData output_data(output_samples, 0, layout, 8);
    std::array<iv::InputPort, 1> inputs{iv::InputPort(input_data, 0)};
    std::array<iv::OutputPort, 1> outputs{iv::OutputPort(output_data, 0)};

    for (size_t frame = 0; frame < 4; ++frame) {
        input_samples[input_data.sample_index(frame, 0)] =
            iv::Sample{static_cast<float>(frame + 1)};
        input_samples[input_data.sample_index(frame, 1)] =
            iv::Sample{static_cast<float>(frame + 11)};
    }

    iv::do_tick_block(Node{}, iv::TickBlockContext<Node>{
        iv::TickContext<Node>{
            .inputs = inputs,
            .outputs = outputs,
            .event_inputs = {},
            .event_outputs = {},
            .sample_rate = 48000,
            .scc_feedback_latency = 0,
            .buffer = {},
        },
        0,
        4,
    });

    for (size_t frame = 0; frame < 4; ++frame) {
        EXPECT_EQ(
            output_samples[output_data.sample_index(frame, 0)],
            iv::Sample{static_cast<float>(frame + 1)});
        EXPECT_EQ(
            output_samples[output_data.sample_index(frame, 1)],
            iv::Sample{static_cast<float>(frame + 11)});
    }
}

iv::ConnectionNode tracked_connection_node()
{
    const auto mono_planar = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    return iv::details::make_generated_node(iv::ConnectionNodeSpec{
        .input_configs = {
            iv::ConnectionNodeInputConfig{
                .input = iv::SampleInputConfig{.channel_layout = mono_planar},
                .channel_copies = {{
                    .input_channel = 0,
                    .ephemeral_port = 0,
                    .ephemeral_channel = 0,
                }},
            },
        },
        .ephemeral_port_configs = {
            iv::ConnectionNodeEphemeralPortConfig{
                .channel_layout = mono_planar,
                .conversion = iv::ChannelConversionPlan{
                    .source = mono_planar,
                    .target = mono_planar,
                    .convert = &tracked_mono_block_copy,
                },
                .output_channel_copies = {{
                    .converted_channel = 0,
                    .output_channel = 0,
                }},
            },
        },
        .output_config = iv::SampleOutputConfig{.channel_layout = mono_planar},
        .default_value = iv::Sample{0},
    });
}

struct ChannelTopologySnapshot {
    bool ok = false;
    size_t connection_nodes = 0;
};

struct StaticConstantFanoutSnapshot {
    size_t wrapped_node_count = 0;
    size_t constant_wrapper_count = 0;
    size_t static_owner_count = 0;
    size_t static_alias_count = 0;
};

iv::ConfiguredGraphTestView configure_boundary_adapter()
{
    iv::GraphBuilder g;
    (void)iv::details::configure_concrete_node<iv::ChannelPack<iv::stereo>>(g);
    (void)iv::details::configure_concrete_node<iv::ChannelUnpack<iv::stereo>>(g);
    g.outputs();
    return iv::freeze_configured_graph_for_test(std::move(g).finish());
}

ChannelTopologySnapshot boundary_adapter_snapshot()
{
    auto const built = compile_graph(configure_boundary_adapter());
    return {.ok = built.graph.outputs().empty()};
}

iv::ConfiguredGraphTestView configure_tiled_source()
{
    iv::GraphBuilder g;
    auto source = iv::details::configure_concrete_tiled_node<iv::Constant, iv::stereo>(
        g, iv::Sample{0.25f});
    auto stream = source.get<0>();
    auto left = stream[iv::stereo::left];

    static_assert(std::same_as<decltype(stream),
        iv::TypedSamplePortTileRef<iv::stereo>>);
    static_assert(std::same_as<decltype(left),
        iv::TypedSamplePortTileChannelRef<
            iv::stereo,
            std::remove_cvref_t<decltype(iv::stereo::left)>>>);

    g.outputs(iv::PortName<"left">{} = left);
    return iv::freeze_configured_graph_for_test(std::move(g).finish());
}

ChannelTopologySnapshot tiled_source_snapshot()
{
    auto const built = compile_graph(configure_tiled_source());
    return {
        .ok = built.graph.outputs().size() == 1
            && iv::sample_properties(built.graph.outputs().front()).channel_layout.channel_type
                == iv::ChannelTypeId::mono,
    };
}

struct SampleRefConfiguration {
    iv::ConfiguredGraphTestView view;
    bool configured_ok;
};

SampleRefConfiguration configure_sample_ref()
{
    iv::GraphBuilder g;
    auto source = iv::details::configure_concrete_node<NamedStereoSource>(
        g, iv::Sample{0.25f}, iv::Sample{-0.5f});
    auto stream = source[iv::PortName<"main">{}];
    auto erased = static_cast<iv::SamplePortRef>(stream);
    auto left = static_cast<iv::SamplePortRef>(stream[iv::stereo::left]);

    bool ok = erased.channel_type == iv::ChannelTypeId::stereo
        && erased.channels().size() == 2
        && erased.channels()[0].bundle == source.node_bundle_handle()
        && erased.channels()[0].port == 0
        && erased.channels()[0].channel == 0
        && erased.channels()[1].bundle == source.node_bundle_handle()
        && erased.channels()[1].port == 0
        && erased.channels()[1].channel == 1
        && left.channel_type == iv::ChannelTypeId::mono
        && left.channels().size() == 1
        && left.channels().front() == erased.channels().front();

    g.outputs();
    return {
        .view = iv::freeze_configured_graph_for_test(std::move(g).finish()),
        .configured_ok = ok,
    };
}

ChannelTopologySnapshot sample_ref_snapshot()
{
    auto const configured = configure_sample_ref();
    auto const built = compile_graph(configured.view);
    return {
        .ok = configured.configured_ok
            && !has_generated_type(
                built.metadata.concrete_node_type_identities, "ChannelUnpack")
            && !has_generated_type(
                built.metadata.concrete_node_type_identities, "ConnectionNode"),
    };
}

struct StructuralTileConfiguration {
    iv::ConfiguredGraphTestView view;
    bool configured_ok;
};

StructuralTileConfiguration configure_structural_tile()
{
    iv::GraphBuilder g;
    auto left = iv::details::configure_concrete_node<iv::Constant>(
        g, iv::Sample{0.25f});
    auto right = iv::details::configure_concrete_node<iv::Constant>(
        g, iv::Sample{-0.5f});
    auto tiled = g.tile<iv::stereo>(left, right);
    auto erased = static_cast<iv::SamplePortRef>(tiled);

    bool ok = erased.channel_type == iv::ChannelTypeId::stereo
        && erased.channels().size() == 2
        && erased.channels()[0].bundle == left.node_bundle_handle()
        && erased.channels()[1].bundle == right.node_bundle_handle();

    g.outputs();
    return {
        .view = iv::freeze_configured_graph_for_test(std::move(g).finish()),
        .configured_ok = ok,
    };
}

ChannelTopologySnapshot structural_tile_snapshot()
{
    auto const configured = configure_structural_tile();
    auto const built = compile_graph(configured.view);
    return {
        .ok = configured.configured_ok
            && !has_generated_type(
                built.metadata.concrete_node_type_identities, "ChannelPack")
            && !has_generated_type(
                built.metadata.concrete_node_type_identities, "ChannelUnpack")
            && !has_generated_type(
                built.metadata.concrete_node_type_identities, "ConnectionNode"),
    };
}

struct QualifiedOutputConfiguration {
    iv::ConfiguredGraphTestView view;
    size_t after_outputs_handle;
    size_t right_handle;
};

QualifiedOutputConfiguration configure_qualified_output()
{
    iv::GraphBuilder g;
    auto left = iv::details::configure_concrete_node<iv::Constant>(
        g, iv::Sample{0.25f});
    auto right = iv::details::configure_concrete_node<iv::Constant>(
        g, iv::Sample{-0.5f});
    g.outputs(
        iv::PortName<"main">{}[iv::stereo::left] =
            static_cast<iv::SamplePortRef>(left),
        iv::PortName<"main">{}[iv::stereo::right] =
            static_cast<iv::SamplePortRef>(right));

    auto after_outputs = iv::details::configure_concrete_node<iv::Constant>(
        g, iv::Sample{1.0f});
    return {
        .view = iv::freeze_configured_graph_for_test(std::move(g).finish()),
        .after_outputs_handle = after_outputs.node_bundle_handle(),
        .right_handle = right.node_bundle_handle(),
    };
}

ChannelTopologySnapshot qualified_output_snapshot()
{
    auto const configured = configure_qualified_output();
    auto const built = compile_graph(configured.view);
    auto const connection_nodes = std::ranges::count_if(
        built.metadata.concrete_node_type_identities,
        [](auto const& type) {
            return std::string_view(type).contains("ConnectionNode");
        });
    return {
        .ok = configured.after_outputs_handle == configured.right_handle + 1
            && built.graph.outputs().size() == 1
            && iv::sample_properties(built.graph.outputs().front()).channel_layout.channel_type
                == iv::ChannelTypeId::stereo
            && !has_generated_type(
                built.metadata.concrete_node_type_identities, "ChannelPack")
            && !has_generated_type(
                built.metadata.concrete_node_type_identities, "ChannelUnpack"),
        .connection_nodes = static_cast<size_t>(connection_nodes),
    };
}

struct DetachConfiguration {
    iv::ConfiguredGraphTestView view;
    size_t after_detach_handle;
    size_t right_handle;
};

DetachConfiguration configure_detach()
{
    iv::GraphBuilder g;
    auto left = iv::details::configure_concrete_node<iv::Constant>(
        g, iv::Sample{0.25f});
    auto right = iv::details::configure_concrete_node<iv::Constant>(
        g, iv::Sample{-0.5f});
    auto structural = g.tile<iv::stereo>(left, right);
    auto detached = static_cast<iv::SamplePortRef>(structural).detach();
    auto after_detach = iv::details::configure_concrete_node<iv::Constant>(
        g, iv::Sample{1.0f});
    g.outputs(detached);
    return {
        .view = iv::freeze_configured_graph_for_test(std::move(g).finish()),
        .after_detach_handle = after_detach.node_bundle_handle(),
        .right_handle = right.node_bundle_handle(),
    };
}

ChannelTopologySnapshot detach_snapshot()
{
    auto const configured = configure_detach();
    auto graph = iv::thaw_configured_graph_for_test(configured.view);
    auto const connections = graph.connections.configured_sample_connections();
    return {
        .ok = configured.after_detach_handle == configured.right_handle + 1
            && graph.node_bundles.size() == 4
            && connections.size() == 1
            && connections.front().source_type == iv::ChannelTypeId::stereo
            && connections.front().source_channels.size() == 2
            && connections.front().detach.has_value()
            && connections.front().detach->loop_extra_latency == 1
            && !connections.front().detach->initial_value_override.has_value(),
        .connection_nodes = 0,
    };
}

struct TiledEventConfiguration {
    iv::ConfiguredGraphTestView view;
    size_t tiled_handle;
    bool tiled_event_input_connected;
    bool ok;
    bool merged_ok;
    bool virtual_event_input_ok;
    bool virtual_event_output_ok;
};

TiledEventConfiguration configure_tiled_event()
{
    iv::GraphBuilder g;
    auto tiled = iv::_annotate_node_source_info(
        iv::details::configure_concrete_tiled_node<EventfulMonoPass, iv::stereo>(g), "eventful-tiled");
    auto source = iv::details::configure_concrete_node<iv::EventConcatenation>(
        g, 0, iv::EventTypeId::trigger);
    tiled("trigger"_F = source.event_port(0));
    auto merged = tiled.event_port(0);
    auto sink = iv::details::configure_concrete_node<iv::EventConcatenation>(
        g, 1, iv::EventTypeId::trigger);
    sink.connect_event_input(0, merged);
    g.outputs();

    auto const virtual_ports = iv::host::virtual_ports(g);
    auto const tiled_handle = tiled.node_bundle_handle();
    auto const tiled_event_input_connected = tiled.event_input_is_connected(0);
    bool const merged_ok = merged.sources().size() == 1
        && merged.sources().front().bundle == tiled_handle;
    bool const virtual_ok = virtual_ports.event_inputs.size() == 1
        && virtual_ports.event_outputs.size() == 1
        && virtual_ports.event_inputs.front().node_bundle_ports.size() == 1
        && virtual_ports.event_outputs.front().node_bundle_ports.size() == 1
        && virtual_ports.event_inputs.front().config.type
            == iv::EventTypeId::trigger;
    return {
        .view = iv::freeze_configured_graph_for_test(std::move(g).finish()),
        .tiled_handle = tiled_handle,
        .tiled_event_input_connected = tiled_event_input_connected,
        .ok = merged_ok,
        .merged_ok = merged_ok,
        .virtual_event_input_ok = virtual_ok,
        .virtual_event_output_ok = virtual_ok,
    };
}

ChannelTopologySnapshot tiled_event_snapshot()
{
    auto const configured = configure_tiled_event();
    auto const built = compile_graph(configured.view);
    auto const merge_count = std::ranges::count_if(
        built.metadata.concrete_node_type_identities,
        [](auto const& type) {
            return std::string_view(type).contains("EventConcatenation");
        });
    return {
        .ok = configured.merged_ok
            && configured.tiled_event_input_connected
            && configured.virtual_event_input_ok
            && configured.virtual_event_output_ok
            && merge_count == 3,
    };
}

TEST(Channels, TiledNodePositionalArgumentsFollowMixedDeclarationOrder)
{
    iv::GraphBuilder g;
    auto node = iv::details::configure_concrete_tiled_node<
        EventBeforeSamplePass, iv::stereo>(g);
    auto event_source = iv::details::configure_concrete_node<
        iv::EventConcatenation>(g, 0, iv::EventTypeId::trigger);
    auto sample_source = iv::details::configure_concrete_node<iv::Constant>(
        g, iv::Sample{0.25f});

    node(event_source.event_port(), sample_source);

    EXPECT_TRUE(node.event_input_is_connected(0));
    EXPECT_TRUE(node.input_is_connected(0));
}

ChannelTopologySnapshot annotation_snapshot()
{
    iv::GraphBuilder g;
    auto node = iv::_annotate_node_source_info(
        iv::details::configure_concrete_tiled_node<MonoPass, iv::stereo>(g), "tiled-pass");
    auto const inputs = iv::host::virtual_sample_input_families(g);
    auto const outputs = iv::host::virtual_sample_output_families(g);
    bool ok = inputs.families.size() == 1
        && outputs.families.size() == 1;
    if (!ok) return {.ok = false};

    auto const& input = inputs.families.front();
    auto const& output = outputs.families.front();
    ok = input.virtual_node_id.starts_with("tiled-pass#type:")
        && input.channel_type == iv::ChannelTypeId::stereo
        && input.channels.size() == 2
        && input.channels[0].targets.size() == 1
        && input.channels[1].targets.size() == 1
        && input.channels[0].targets.front().bundle == node.node_bundle_handle()
        && input.channels[1].targets.front().bundle == node.node_bundle_handle()
        && input.channels[0].targets.front() != input.channels[1].targets.front()
        && output.virtual_node_id == input.virtual_node_id
        && output.channel_type == iv::ChannelTypeId::stereo
        && output.channels.size() == 2
        && output.channels[0].sources.size() == 1
        && output.channels[1].sources.size() == 1;
    return {.ok = ok};
}

iv::ConfiguredGraphTestView configure_introspection()
{
    iv::GraphBuilder g;
    (void)iv::_annotate_node_source_info(
        iv::details::configure_concrete_tiled_node<MonoPass, iv::stereo>(g),
        "tiled-introspection",
        "/tmp/tiled-module.cpp",
        40,
        55);
    return iv::freeze_configured_graph_for_test(std::move(g).finish());
}

ChannelTopologySnapshot introspection_snapshot()
{
    auto const metadata = compile_graph(configure_introspection()).introspection;
    bool ok = metadata.virtual_nodes.size() == 1;
    if (!ok) return {.ok = false};
    auto const& virtual_node = metadata.virtual_nodes.front();
    ok = virtual_node.sample_inputs.size() == 1
        && virtual_node.sample_outputs.size() == 1
        && virtual_node.members.size() == 1
        && virtual_node.source_spans.size() == 1
        && virtual_node.source_spans.front().file_path
            == "/tmp/tiled-module.cpp"
        && virtual_node.members.front().ordinal == 0
        && virtual_node.members.front().backing_node_id.starts_with("node-bundle:")
        && virtual_node.members.front().sample_inputs.size() == 1
        && virtual_node.members.front().sample_outputs.size() == 1
        && virtual_node.members.front().sample_inputs.front().sample_channel_type
            == iv::ChannelTypeId::stereo
        && virtual_node.members.front().sample_outputs.front().sample_channel_type
            == iv::ChannelTypeId::stereo
        && virtual_node.sample_inputs.front().sample_channel_type
            == iv::ChannelTypeId::stereo
        && virtual_node.sample_outputs.front().sample_channel_type
            == iv::ChannelTypeId::stereo;
    return {.ok = ok};
}

ChannelTopologySnapshot typed_operator_snapshot()
{
    iv::GraphBuilder g;
    auto planar_source = iv::details::configure_concrete_node<NamedStereoSource>(g);
    auto interleaved_source = iv::details::configure_concrete_node<NamedInterleavedStereoSource>(g);
    auto planar = planar_source[iv::PortName<"main">{}];
    auto interleaved = interleaved_source[iv::PortName<"main">{}];
    auto same_layout_sum = planar + planar;
    auto mixed_layout_sum = planar + interleaved;
    auto scalar_sum = 1.0f + planar;
    auto scaled = planar * 0.1f;
    auto reverse_scaled = 0.1f * interleaved;
    auto detached = ~planar;

    static_assert(iv::TypedSamplePortLike<decltype(same_layout_sum)>);
    static_assert(iv::TypedSamplePortLike<decltype(mixed_layout_sum)>);
    static_assert(iv::TypedSamplePortLike<decltype(scalar_sum)>);
    static_assert(iv::TypedSamplePortLike<decltype(scaled)>);
    static_assert(iv::TypedSamplePortLike<decltype(reverse_scaled)>);
    static_assert(std::same_as<
        typename iv::typed_sample_port_traits<
            decltype(scaled)>::channel_type,
        iv::stereo>);
    static_assert(std::same_as<
        decltype(detached),
        iv::TypedSamplePortRef<iv::stereo>>);

    auto const scaled_erased = static_cast<iv::SamplePortRef>(scaled);
    auto const reverse_scaled_erased =
        static_cast<iv::SamplePortRef>(reverse_scaled);
    return {
        .ok = scaled_erased.channel_type == iv::ChannelTypeId::stereo
            && scaled_erased.channels().size() == 2
            && reverse_scaled_erased.channel_type == iv::ChannelTypeId::stereo
            && reverse_scaled_erased.channels().size() == 2,
    };
}

struct StereoScalarProductConfiguration {
    iv::ConfiguredGraphTestView view;
    bool configured_ok;
};

StereoScalarProductConfiguration configure_stereo_scalar_product()
{
    iv::GraphBuilder g;
    auto source = iv::details::configure_concrete_node<NamedStereoSource>(
        g, iv::Sample{0.25f}, iv::Sample{-0.5f});
    auto modulation = iv::details::configure_concrete_tiled_node<MonoPass, iv::stereo>(g);
    auto stream = source[iv::PortName<"main">{}];
    auto scaled = stream * 0.1f * modulation;
    g.outputs(scaled);
    auto const public_outputs = iv::host::public_sample_output_families(g);
    bool ok = public_outputs.families.size() == 1
        && public_outputs.families.front().channel_type
            == iv::ChannelTypeId::stereo
        && public_outputs.families.front().channels.size() == 2;
    return {
        .view = iv::freeze_configured_graph_for_test(std::move(g).finish()),
        .configured_ok = ok,
    };
}

ChannelTopologySnapshot stereo_scalar_product_snapshot()
{
    auto const configured = configure_stereo_scalar_product();
    auto const built = compile_graph(configured.view);
    if (!configured.configured_ok) return {.ok = false};
    if (built.graph.outputs().size() != 1) {
        return {.ok = false, .connection_nodes = 4};
    }
    if (iv::sample_properties(built.graph.outputs().front()).channel_layout.channel_type
        != iv::ChannelTypeId::stereo) {
        return {.ok = false, .connection_nodes = 5};
    }
    return {.ok = true};
}

iv::ConfiguredGraphTestView configure_reconstructed_sequence_reversed()
{
    iv::GraphBuilder g;
    auto source = iv::details::configure_concrete_node<NamedStereoSource>(
        g, iv::Sample{0.25f}, iv::Sample{-0.5f});
    auto stream = source[iv::PortName<"main">{}];
    auto erased = static_cast<iv::SamplePortRef>(stream);
    auto reconstructed = iv::SamplePortRef(
        g, iv::ChannelTypeId::stereo,
        std::vector<iv::SampleOutputChannelId>{
            erased.channels()[1], erased.channels()[0]});
    auto pass = iv::details::configure_concrete_node<
        iv::Sum<iv::stereo, iv::SampleStreamLayout::planar, 1>>(g);
    pass(reconstructed);
    g.outputs(pass);
    return iv::freeze_configured_graph_for_test(std::move(g).finish());
}

iv::ConfiguredGraphTestView configure_reconstructed_sequence_ordered()
{
    iv::GraphBuilder g;
    auto source = iv::details::configure_concrete_node<NamedStereoSource>(
        g, iv::Sample{0.25f}, iv::Sample{-0.5f});
    auto stream = source[iv::PortName<"main">{}];
    auto erased = static_cast<iv::SamplePortRef>(stream);
    auto reconstructed = iv::SamplePortRef(
        g, iv::ChannelTypeId::stereo,
        std::vector<iv::SampleOutputChannelId>{
            erased.channels()[0], erased.channels()[1]});
    auto pass = iv::details::configure_concrete_node<
        iv::Sum<iv::stereo, iv::SampleStreamLayout::planar, 1>>(g);
    pass(reconstructed);
    g.outputs(pass);
    return iv::freeze_configured_graph_for_test(std::move(g).finish());
}

ChannelTopologySnapshot reconstructed_sequence_snapshot(bool reverse)
{
    auto const built = compile_graph(reverse
        ? configure_reconstructed_sequence_reversed()
        : configure_reconstructed_sequence_ordered());
    auto const connection_nodes = std::ranges::count_if(
        built.metadata.concrete_node_type_identities,
        [](auto const& type) {
            return std::string_view(type).contains("ConnectionNode");
        });
    return {
        .ok = true,
        .connection_nodes = static_cast<size_t>(connection_nodes),
    };
}

iv::ConfiguredGraphTestView configure_tiled_mono_direct_route()
{
    iv::GraphBuilder g;
    auto source = iv::details::configure_concrete_node<iv::Constant>(
        g, iv::Sample{0.25f});
    auto target = iv::details::configure_concrete_tiled_node<MonoPass, iv::stereo>(g);
    target(source);
    g.outputs(target);
    return iv::freeze_configured_graph_for_test(std::move(g).finish());
}

ChannelTopologySnapshot tiled_mono_direct_route_snapshot()
{
    auto const built = compile_graph(configure_tiled_mono_direct_route());
    auto const connection_nodes = std::ranges::count_if(
        built.metadata.concrete_node_type_identities,
        [](auto const& type) {
            return std::string_view(type).contains("ConnectionNode");
        });
    return {
        .ok = true,
        .connection_nodes = static_cast<size_t>(connection_nodes),
    };
}

iv::ConfiguredGraphTestView configure_static_constant_fanout()
{
    iv::GraphBuilder g;
    auto source = iv::details::configure_concrete_node<iv::Constant>(
        g, iv::Sample{0.25f});
    auto first = iv::details::configure_concrete_node<MonoPass>(g);
    auto second = iv::details::configure_concrete_node<MonoPass>(g);
    first(source);
    second(source);
    g.outputs(
        iv::PortName<"first">{} = first,
        iv::PortName<"second">{} = second);
    return iv::freeze_configured_graph_for_test(std::move(g).finish());
}

StaticConstantFanoutSnapshot static_constant_fanout_uses_one_initialized_buffer_owner()
{
    auto const built = compile_graph(configure_static_constant_fanout());
    StaticConstantFanoutSnapshot result {};
    if (has_generated_type(
            built.metadata.concrete_node_type_identities, "Broadcast")) {
        return result;
    }
    for (auto const& scc : built.graph._scc_wrappers) {
        result.wrapped_node_count += scc._nodes.size();
        for (size_t node_i = 0; node_i < scc._nodes.size(); ++node_i) {
            auto const& node = scc._nodes[node_i];
            if (scc._global_node_indices[node_i] == 0) {
                ++result.constant_wrapper_count;
            }
            if (node._input_port_data_nodes.size() == 1
                && node._input_port_data_nodes[0]._is_static_constant) {
                if (node._input_port_data_nodes[0]._owns_storage) {
                    ++result.static_owner_count;
                } else {
                    ++result.static_alias_count;
                }
            }
        }
    }
    return result;
}

bool sample_lowering_plan_groups_connections_by_target_port()
{
    iv::GraphBuilderConnections connections;
    connections.record_configured_sample_connection({
        .source_type = iv::ChannelTypeId::mono,
        .source_channels = {{.bundle = 1, .port = 0, .channel = 0}},
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {{.bundle = 4, .port = 2, .channel = 0}},
    });
    connections.record_configured_sample_connection({
        .source_type = iv::ChannelTypeId::mono,
        .source_channels = {{.bundle = 2, .port = 1, .channel = 0}},
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {{.bundle = 4, .port = 2, .channel = 0}},
    });
    connections.record_configured_sample_connection({
        .source_type = iv::ChannelTypeId::mono,
        .source_channels = {{.bundle = 3, .port = 0, .channel = 0}},
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {{.bundle = 5, .port = 0, .channel = 0}},
    });
    auto const plan = connections.sample_lowering_plan();
    return plan.groups.size() == 2
        && plan.groups[0].target == iv::NodeBundlePortId{4, iv::PortKind::sample, 2}
        && plan.groups[0].connections.size() == 2
        && plan.groups[1].target == iv::NodeBundlePortId{5, iv::PortKind::sample, 0}
        && plan.groups[1].connections.size() == 1;
}

template<bool Connected>
iv::ConfiguredGraphTestView configure_connection_lowering()
{
    iv::GraphBuilder g;
    auto pass = iv::details::configure_concrete_node<DefaultMonoPass>(g);
    if constexpr (Connected) {
        pass(iv::details::configure_concrete_node<iv::Constant>(g, iv::Sample{0.25f}));
        pass(iv::details::configure_concrete_node<iv::Constant>(g, iv::Sample{-0.5f}));
    }
    g.outputs(pass);
    return iv::freeze_configured_graph_for_test(std::move(g).finish());
}

ChannelTopologySnapshot connection_lowering_snapshot(bool connected)
{
    auto const built = compile_graph(connected
        ? configure_connection_lowering<true>()
        : configure_connection_lowering<false>());
    return {
        .ok = true,
        .connection_nodes = static_cast<size_t>(std::ranges::count_if(
            built.metadata.concrete_node_type_identities,
            [](auto const& type) {
                return std::string_view(type).contains("ConnectionNode");
            })),
    };
}

template<bool Connected>
iv::ConfiguredGraphTestView configure_sample_lowering_pass_graph()
{
    iv::GraphBuilder g;
    auto pass = iv::details::configure_concrete_node<DefaultMonoPass>(g);
    if constexpr (Connected)
        pass(iv::details::configure_concrete_node<iv::Constant>(g, iv::Sample{0.25f}));
    g.outputs(pass);
    return iv::freeze_configured_graph_for_test(std::move(g).finish());
}

iv::details::SampleLoweringPassFacts
sample_lowering_pass_facts(bool connected)
{
    return iv::details::GraphLowererTestAccess::sample_lowering_pass_facts(
        iv::thaw_configured_graph_for_test(
            connected
            ? configure_sample_lowering_pass_graph<true>()
            : configure_sample_lowering_pass_graph<false>()));
}

struct ExecutionRootConfiguration {
    iv::ConfiguredGraphTestView view;
};

ExecutionRootConfiguration configure_execution_root()
{
    iv::GraphBuilder g;
    auto input = g.input<"in">(iv::Sample{-1.0f});
    auto pass = iv::details::configure_concrete_node<MonoPass>(g);
    pass(input);
    g.outputs(iv::PortName<"main">{} = pass);
    return {.view = iv::freeze_configured_graph_for_test(std::move(g).finish())};
}

struct ExecutionRootSnapshot {
    bool closed_interface = false;
    bool has_runtime_sample_input = false;
    bool has_runtime_sample_output = false;
};

ExecutionRootSnapshot execution_root_snapshot()
{
    auto const built = compile_graph(configure_execution_root().view, true);
    return {
        .closed_interface = built.graph.inputs().empty()
            && built.graph.outputs().empty(),
        .has_runtime_sample_input = has_generated_type(
            built.metadata.concrete_node_type_identities,
            "RuntimeSampleInputNode"),
        .has_runtime_sample_output = has_generated_type(
            built.metadata.concrete_node_type_identities,
            "RuntimeSampleOutput"),
    };
}

TEST(Channels, SamplePortStorageViewConstructsFacadesWithoutSharedPortData)
{
    std::array<iv::Sample, 8> samples{};
    iv::SamplePortStorageView storage{
        std::span<iv::Sample>{samples},
        0,
        iv::mono_planar_channel_layout,
        8,
    };
    // Invocation-local façades may be reconstructed at an arbitrary absolute
    // sample index; no persistent cursor state is required between calls.
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

TEST(Channels, SamplePortStorageViewSupportsDiscontiguousPlanarChannels)
{
    std::array<iv::Sample, 8> left{};
    std::array<iv::Sample, 8> right{};
    std::array<
        iv::SampleChannelStorageView,
        iv::maximum_supported_channel_count> channels{};
    channels[0] = iv::SampleChannelStorageView{
        .storage = left.data(),
        .frame_capacity = 8,
        .frame_stride = 1,
    };
    channels[1] = iv::SampleChannelStorageView{
        .storage = right.data(),
        .frame_capacity = 8,
        .frame_stride = 1,
    };
    iv::ChannelLayout const layout{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::SamplePortStorageView storage{channels, 0, layout, 8};
    iv::OutputPort output(storage, 0, 6);
    iv::InputPort input(storage, 0, 0, 6);

    output.write_frame(0, 0, 1.25f);
    output.write_frame(0, 1, 2.5f);

    EXPECT_FLOAT_EQ(left[6], 1.25f);
    EXPECT_FLOAT_EQ(right[6], 2.5f);
    EXPECT_FLOAT_EQ(input.get_frame(0, 0), 1.25f);
    EXPECT_FLOAT_EQ(input.get_frame(0, 1), 2.5f);
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
    channels[0] = iv::SampleChannelStorageView{
        .storage = interleaved.data(),
        .frame_capacity = 8,
        .frame_stride = 2,
    };
    channels[1] = iv::SampleChannelStorageView{
        .storage = interleaved.data() + 1,
        .frame_capacity = 8,
        .frame_stride = 2,
    };
    iv::SamplePortStorageView storage{
        channels,
        0,
        iv::ChannelLayout{
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

TEST(Channels, OutputPortUpdateRevisesOnlyUnpublishedLatencyWindow)
{
    std::array<iv::Sample, 8> samples{};
    iv::SamplePortStorageView storage{
        std::span<iv::Sample>{samples},
        0,
        iv::mono_planar_channel_layout,
        8,
    };
    iv::OutputPort output(storage, 0, 0, 2);

    output.push(iv::Sample{1.0f});
    output.push(iv::Sample{2.0f});
    output.update(iv::Sample{20.0f});
    output.update(iv::Sample{10.0f}, 1);
    output.update(iv::Sample{99.0f}, 2);

    EXPECT_FLOAT_EQ(samples[0], 10.0f);
    EXPECT_FLOAT_EQ(samples[1], 20.0f);
    EXPECT_FLOAT_EQ(output.get(0), 20.0f);
    EXPECT_FLOAT_EQ(output.get(1), 10.0f);
}

TEST(Channels, TickOnlyBlockFallbackAdvancesSequentialPortCursors)
{
    std::array<iv::Sample, 16> input_samples{};
    std::array<iv::Sample, 16> output_samples{};
    for (std::size_t i = 0; i < input_samples.size(); ++i) {
        input_samples[i] = static_cast<iv::Sample>(10 * i);
    }
    output_samples[4] = iv::Sample{400.0f};

    iv::SharedPortData input_data(input_samples, 0);
    iv::SharedPortData output_data(output_samples, 0);
    std::array<iv::InputPort, 1> inputs{
        iv::InputPort(input_data, 1, 0, 5)};
    std::array<iv::OutputPort, 1> outputs{
        iv::OutputPort(output_data, 1, 5, 2)};
    TickFallbackContractTrace trace;

    iv::do_tick_block(
        TickFallbackContractProbe{&trace},
        iv::TickBlockContext<TickFallbackContractProbe>{
            iv::TickContext<TickFallbackContractProbe>{
                .inputs = inputs,
                .outputs = outputs,
            },
            5,
            4,
        });

    EXPECT_EQ(trace.calls, 4u);
    EXPECT_EQ(
        trace.indices,
        (std::array<iv::SampleIndex, 4>{5, 6, 7, 8}));
    EXPECT_EQ(
        trace.current_inputs,
        (std::array<iv::Sample, 4>{50.0f, 60.0f, 70.0f, 80.0f}));
    EXPECT_EQ(
        trace.history_inputs,
        (std::array<iv::Sample, 4>{40.0f, 50.0f, 60.0f, 70.0f}));
    EXPECT_EQ(
        trace.previous_outputs,
        (std::array<iv::Sample, 4>{400.0f, 5.0f, 6.0f, 7.0f}));
    EXPECT_EQ(
        trace.output_positions,
        (std::array<std::size_t, 4>{5, 6, 7, 8}));

    EXPECT_FLOAT_EQ(output_samples[5], 500.0f);
    EXPECT_FLOAT_EQ(output_samples[6], 6.0f);
    EXPECT_FLOAT_EQ(output_samples[7], 7.0f);
    EXPECT_FLOAT_EQ(output_samples[8], 8.0f);
    EXPECT_EQ(outputs[0].position(), 9u);
    EXPECT_FLOAT_EQ(inputs[0].get(), 90.0f);
    EXPECT_FLOAT_EQ(inputs[0].get(1), 80.0f);
}

TEST(Channels, TickBlockKeepsInputsAnchoredAndAdvancesSequentialOutputs)
{
    std::array<iv::Sample, 16> input_samples{};
    std::array<iv::Sample, 16> output_samples{};
    for (std::size_t i = 0; i < input_samples.size(); ++i) {
        input_samples[i] = static_cast<iv::Sample>(10 * i);
    }
    output_samples[4] = iv::Sample{400.0f};

    iv::SharedPortData input_data(input_samples, 0);
    iv::SharedPortData output_data(output_samples, 0);
    std::array<iv::InputPort, 1> inputs{
        iv::InputPort(input_data, 1, 0, 5)};
    std::array<iv::OutputPort, 1> outputs{
        iv::OutputPort(output_data, 0, 5, 2)};
    TickBlockContractTrace trace;

    iv::do_tick_block(
        TickBlockContractProbe{&trace},
        iv::TickBlockContext<TickBlockContractProbe>{
            iv::TickContext<TickBlockContractProbe>{
                .inputs = inputs,
                .outputs = outputs,
            },
            5,
            4,
        });

    EXPECT_EQ(trace.index, 5u);
    EXPECT_EQ(trace.block_size, 4u);
    EXPECT_FLOAT_EQ(trace.anchored_input, 50.0f);
    EXPECT_EQ(
        trace.block_inputs,
        (std::array<iv::Sample, 4>{50.0f, 60.0f, 70.0f, 80.0f}));
    EXPECT_FLOAT_EQ(trace.previous_output, 400.0f);
    EXPECT_EQ(trace.output_position_before, 5u);
    EXPECT_EQ(trace.output_position_after, 9u);

    EXPECT_FLOAT_EQ(output_samples[4], 444.0f);
    EXPECT_FLOAT_EQ(output_samples[5], 50.0f);
    EXPECT_FLOAT_EQ(output_samples[6], 60.0f);
    EXPECT_FLOAT_EQ(output_samples[7], 70.0f);
    EXPECT_FLOAT_EQ(output_samples[8], 80.0f);
    EXPECT_EQ(outputs[0].position(), 9u);
    EXPECT_FLOAT_EQ(inputs[0].get(), 90.0f);
}

TEST(Channels, DirectBlockWritesCommitOutputCursorAfterCallback)
{
    std::array<iv::Sample, 16> output_samples{};
    iv::SharedPortData output_data(output_samples, 0);
    std::array<iv::OutputPort, 1> outputs{
        iv::OutputPort(output_data, 0, 5)};
    DirectBlockWriteContractTrace trace;

    iv::do_tick_block(
        DirectBlockWriteContractProbe{&trace},
        iv::TickBlockContext<DirectBlockWriteContractProbe>{
            iv::TickContext<DirectBlockWriteContractProbe>{
                .outputs = outputs,
            },
            5,
            4,
        });

    EXPECT_EQ(trace.output_position_before, 5u);
    EXPECT_EQ(trace.output_position_after_write, 5u);
    EXPECT_EQ(outputs[0].position(), 9u);
    EXPECT_FLOAT_EQ(output_samples[5], 5.0f);
    EXPECT_FLOAT_EQ(output_samples[6], 6.0f);
    EXPECT_FLOAT_EQ(output_samples[7], 7.0f);
    EXPECT_FLOAT_EQ(output_samples[8], 8.0f);
}

TEST(Channels, SynthesizedSkipSilencesAllChannelsAndAdvancesInputs)
{
    constexpr auto stereo = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };
    std::array<iv::Sample, 8> input_samples{};
    std::array<iv::Sample, 16> output_samples{};
    for (std::size_t i = 0; i < input_samples.size(); ++i) {
        input_samples[i] = static_cast<iv::Sample>(i);
    }
    std::ranges::fill(output_samples, iv::Sample{9.0f});

    iv::SharedPortData input_data(input_samples, 0);
    iv::SharedPortData output_data(output_samples, 0, stereo, 8);
    std::array<iv::InputPort, 1> inputs{
        iv::InputPort(input_data, 0, 0, 2)};
    std::array<iv::OutputPort, 1> outputs{
        iv::OutputPort(output_data, 0, 2)};

    iv::do_skip_block(
        SynthesizedStereoSkipProbe{},
        iv::SkipBlockContext<SynthesizedStereoSkipProbe>{
            iv::TickContext<SynthesizedStereoSkipProbe>{
                .inputs = inputs,
                .outputs = outputs,
            },
            2,
            4,
        });

    for (std::size_t frame = 0; frame < 8; ++frame) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            auto const value = output_samples[output_data.sample_index(frame, channel)];
            if (frame >= 2 && frame < 6) {
                EXPECT_FLOAT_EQ(value, 0.0f);
            } else {
                EXPECT_FLOAT_EQ(value, 9.0f);
            }
        }
    }
    EXPECT_EQ(outputs[0].position(), 6u);
    EXPECT_FLOAT_EQ(inputs[0].get(), 6.0f);
}

TEST(Channels, OutputPortGetBlockRejectsOffsetBeyondReadableWindow)
{
    std::array<iv::Sample, 8> samples{};
    iv::SharedPortData data(samples, 0);
    iv::OutputPort output(data, 1, 0, 2);
    output.push(iv::Sample{1.0f});
    output.push(iv::Sample{2.0f});

    EXPECT_TRUE(output.get_block(2, 5).empty());
}

TEST(Channels, OutputPortUpdateAppliesWriteBoundaryConversion)
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
    iv::SamplePortStorageView storage{
        std::span<iv::Sample>{samples},
        0,
        stereo,
        8,
    };
    iv::OutputPort output(
        storage,
        0,
        mono,
        iv::ChannelConversionRegistry::plan(mono, stereo),
        0,
        1);

    output.push(iv::Sample{2.0f});
    output.update(iv::Sample{3.0f});

    EXPECT_FLOAT_EQ(samples[storage.sample_index(0, 0)], 3.0f);
    EXPECT_FLOAT_EQ(samples[storage.sample_index(0, 1)], 3.0f);
}

TEST(Channels, MonoPlanarIdentityConversionPreservesExactSamples)
{
    auto const samples = std::array<iv::Sample, 5>{
        iv::Sample{1.0f}, iv::Sample{-2.0f}, iv::Sample{3.5f},
        iv::Sample{0.25f}, iv::Sample{-0.75f},
    };
    auto const converted = iv::copy_sample_storage_block(
        borrowed_block(
            samples,
            {.channel_type = iv::ChannelTypeId::mono,
             .sample_layout = iv::SampleStreamLayout::planar},
            samples.size()).view(),
        {.channel_type = iv::ChannelTypeId::mono,
         .sample_layout = iv::SampleStreamLayout::planar});
    EXPECT_EQ(converted.frame_count, samples.size());
    EXPECT_EQ(sample_values(converted),
              (std::vector<iv::Sample>(samples.begin(), samples.end())));
}

TEST(Channels, StereoInterleavedIdentityConversionPreservesExactSamples)
{
    auto const samples = std::array<iv::Sample, 8>{
        iv::Sample{1}, iv::Sample{10}, iv::Sample{2}, iv::Sample{20},
        iv::Sample{3}, iv::Sample{30}, iv::Sample{4}, iv::Sample{40},
    };
    auto const converted = iv::copy_sample_storage_block(
        borrowed_block(
            samples,
            {.channel_type = iv::ChannelTypeId::stereo,
             .sample_layout = iv::SampleStreamLayout::interleaved},
            4).view(),
        {.channel_type = iv::ChannelTypeId::stereo,
         .sample_layout = iv::SampleStreamLayout::interleaved});
    EXPECT_EQ(converted.frame_count, 4u);
    EXPECT_EQ(sample_values(converted),
              (std::vector<iv::Sample>(samples.begin(), samples.end())));
}

TEST(Channels, ZeroFrameConversionProducesEmptyStorage)
{
    auto const converted = iv::copy_sample_storage_block(
        borrowed_block(
            std::span<iv::Sample const>{},
            {.channel_type = iv::ChannelTypeId::mono,
             .sample_layout = iv::SampleStreamLayout::planar},
            0).view(),
        {.channel_type = iv::ChannelTypeId::stereo,
         .sample_layout = iv::SampleStreamLayout::interleaved});
    EXPECT_TRUE(converted.samples.empty());
    EXPECT_EQ(converted.frame_count, 0u);
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
        iv::OutputPort(left_data, 0), iv::OutputPort(right_data, 0)};
    using Node = iv::ChannelUnpack<iv::stereo>;
    iv::do_tick(Node{}, iv::TickSampleContext<Node>{
        iv::TickContext<Node>{
            .inputs = inputs,
            .outputs = outputs,
            .event_inputs = {},
            .event_outputs = {},
            .sample_rate = 48000,
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
    constexpr auto mono_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    constexpr auto stereo_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    std::array<iv::Sample, 4> left_samples{iv::Sample{2.0f}};
    std::array<iv::Sample, 4> right_samples{iv::Sample{-3.0f}};
    std::array<iv::Sample, 8> output_samples{};
    iv::SharedPortData left_data(left_samples, 0, mono_layout, 4);
    iv::SharedPortData right_data(right_samples, 0, mono_layout, 4);
    iv::SharedPortData output_data(output_samples, 0, stereo_layout, 4);
    using Node = iv::ChannelPack<iv::stereo>;
    std::array<iv::InputPort, 2> inputs{
        iv::InputPort(left_data, 0), iv::InputPort(right_data, 0)};
    std::array<iv::OutputPort, 1> outputs{iv::OutputPort(output_data, 0)};
    iv::do_tick_block(Node{}, iv::TickBlockContext<Node>{
        iv::TickContext<Node>{
            .inputs = inputs,
            .outputs = outputs,
            .event_inputs = {},
            .event_outputs = {},
            .sample_rate = 48000,
            .scc_feedback_latency = 0,
            .buffer = {},
        },
        0,
        1,
    });
    EXPECT_EQ(output_samples[output_data.sample_index(0, 0)], iv::Sample{2.0f});
    EXPECT_EQ(output_samples[output_data.sample_index(0, 1)], iv::Sample{-3.0f});
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
    iv::OutputPort output(
        data, 0, mono, iv::ChannelConversionRegistry::plan(mono, stereo));
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
    iv::OutputPort output(
        data, 0, stereo, iv::ChannelConversionRegistry::plan(stereo, mono));
    output.push_frame(std::array<iv::Sample, 2>{
        iv::Sample{2.0f}, iv::Sample{6.0f}});
    EXPECT_EQ(samples[data.sample_index(0, 0)], iv::Sample{4.0f});
}

TEST(Channels, ConnectionNodeConvertsEachEphemeralExpressionAsOneBlock)
{
    static auto const connection = tracked_connection_node();
    constexpr auto mono_planar = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    connection_conversion_call_count = 0;
    connection_conversion_frame_count = 0;
    std::array<iv::Sample, 8> input_samples{};
    std::ranges::fill(input_samples, iv::Sample{0.5f});
    std::array<iv::Sample, 8> output_samples{};
    iv::SharedPortData input_data(input_samples, 0, mono_planar, 8);
    iv::SharedPortData output_data(output_samples, 0, mono_planar, 8);
    std::array<iv::InputPort, 1> inputs{iv::InputPort(input_data, 0)};
    std::array<iv::OutputPort, 1> outputs{iv::OutputPort(output_data, 0)};
    std::array<iv::Sample, 8> output_scratch{};
    std::array<iv::Sample, 8> gathered_scratch{};
    std::array<iv::Sample, 8> converted_scratch{};
    alignas(iv::ConnectionNode::State)
        std::array<std::byte, sizeof(iv::ConnectionNode::State)> state_storage{};
    auto* state = std::construct_at(
        reinterpret_cast<iv::ConnectionNode::State*>(state_storage.data()),
        iv::ConnectionNode::State{
            .output = output_scratch,
            .gathered = gathered_scratch,
            .converted = converted_scratch,
        });
    iv::do_tick_block(
        connection,
        iv::TickBlockContext<iv::ConnectionNode>{
            iv::TickContext<iv::ConnectionNode>{
                .inputs = inputs,
                .outputs = outputs,
                .event_inputs = {},
                .event_outputs = {},
                .sample_rate = 48000,
                .scc_feedback_latency = 0,
                .buffer = state_storage,
            },
            0,
            8,
        });
    std::destroy_at(state);
    EXPECT_EQ(connection_conversion_call_count, 1u);
    EXPECT_EQ(connection_conversion_frame_count, 8u);
    EXPECT_EQ(output_samples, input_samples);
}

TEST(Channels, ChannelBoundaryAdaptersInsertAsOrdinaryGraphNodes)
{
    auto snapshot = boundary_adapter_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, FullyMonoNodeTilesIntoAStaticStereoOutput)
{
    auto snapshot = tiled_source_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, SampleRefsExposeOrderedStructuralChannelIdentity)
{
    auto snapshot = sample_ref_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, PortExpressionHandlesAreBuilderOwnedAndValidated)
{
    iv::GraphBuilder source_builder;
    auto sample_node = iv::details::configure_concrete_node<iv::Constant>(
        source_builder, iv::Sample{0.25f});
    auto event_node = iv::details::configure_concrete_node<iv::EventConcatenation>(
        source_builder,
        0, iv::EventTypeId::trigger);
    iv::SamplePortRef sample = sample_node;
    iv::EventPortRef event = event_node.event_port();
    auto const sample_copy = sample;
    auto const event_copy = event;

    EXPECT_NE(sample.handle, std::numeric_limits<size_t>::max());
    EXPECT_NE(event.handle, std::numeric_limits<size_t>::max());
    EXPECT_EQ(sample_copy.handle, sample.handle);
    EXPECT_EQ(event_copy.handle, event.handle);
    EXPECT_TRUE(std::ranges::equal(sample_copy.channels(), sample.channels()));
    EXPECT_TRUE(std::ranges::equal(event_copy.sources(), event.sources()));

    auto invalid_sample = sample;
    invalid_sample.handle = std::numeric_limits<size_t>::max();
    EXPECT_THROW((void)invalid_sample.channels(), std::logic_error);
    auto invalid_event = event;
    invalid_event.handle = std::numeric_limits<size_t>::max();
    EXPECT_THROW((void)invalid_event.sources(), std::logic_error);

    iv::GraphBuilder other_builder;
    EXPECT_THROW(other_builder.outputs(sample), std::logic_error);
    EXPECT_THROW(other_builder.event_outputs(event), std::logic_error);
}

TEST(Channels, GraphBuilderTileIsPureStructuralComposition)
{
    auto snapshot = structural_tile_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, ChannelQualifiedPublicOutputsAreProjectedOnlyAtCompletion)
{
    auto snapshot = qualified_output_snapshot();
    EXPECT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.connection_nodes, 1u);
}

TEST(Channels, TiledDetachStaysConnectionMetadata)
{
    auto snapshot = detach_snapshot();
    EXPECT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.connection_nodes, 0u);
}

bool named_public_inputs_preserve_requested_channel_types()
{
    iv::GraphBuilder graph;
    auto const default_input = graph.input<"default">(iv::Sample{0.25f});
    auto const stereo_input = graph.input<"stereo", iv::stereo>(iv::Sample{0.5f});
    auto const stereo_left = stereo_input[iv::stereo::left];
    auto const scaled_stereo = stereo_input * 0.5f;
    auto const named_stereo_input = iv::PortName<"input">{} = stereo_input;
    auto const named_stereo_channel =
        iv::PortName<"output">{}[iv::stereo::left] = stereo_input;

    static_assert(iv::TypedSamplePortLike<decltype(default_input)>);
    static_assert(iv::TypedSamplePortLike<decltype(stereo_input)>);
    static_assert(std::same_as<
        typename iv::typed_sample_port_traits<
            std::remove_cvref_t<decltype(default_input)>>::channel_type,
        iv::mono>);
    static_assert(std::same_as<
        typename iv::typed_sample_port_traits<
            std::remove_cvref_t<decltype(stereo_input)>>::channel_type,
        iv::stereo>);
    static_assert(iv::TypedSamplePortLike<decltype(scaled_stereo)>);
    static_assert(std::same_as<
        std::remove_cvref_t<decltype(named_stereo_input)>,
        iv::NamedArg<"input", iv::SamplePortRef>>);
    static_assert(std::same_as<
        std::remove_cvref_t<decltype(named_stereo_channel)>,
        iv::ChannelNamedArg<"output", iv::stereo, 0, iv::SamplePortRef>>);

    auto const stereo_left_erased = static_cast<iv::SamplePortRef>(stereo_left);
    auto const inputs = iv::host::public_sample_input_families(graph);

    return static_cast<iv::SamplePortRef>(default_input).channel_type
            == iv::ChannelTypeId::mono
        && static_cast<iv::SamplePortRef>(default_input).channels().size() == 1
        && static_cast<iv::SamplePortRef>(stereo_input).channel_type
            == iv::ChannelTypeId::stereo
        && static_cast<iv::SamplePortRef>(stereo_input).channels().size() == 2
        && stereo_left_erased.channel_type == iv::ChannelTypeId::mono
        && stereo_left_erased.channels().size() == 1
        && inputs.families.size() == 2
        && inputs.families[0].channel_type == iv::ChannelTypeId::mono
        && inputs.families[0].channels.size() == 1
        && inputs.families[1].channel_type == iv::ChannelTypeId::stereo
        && inputs.families[1].channels.size() == 2;
}

TEST(Channels, NamedPublicInputsDefaultToStereoAndAllowExplicitMono)
{
    EXPECT_TRUE(named_public_inputs_preserve_requested_channel_types());
}

TEST(Channels, SubgraphNamedInputsRetainTheirStaticChannelType)
{
    iv::GraphBuilder graph;
    bool input_was_stereo = false;
    graph.subgraph([&](iv::SubgraphBuilder& subgraph) {
        auto const input = subgraph.input<"in", iv::stereo>();
        auto const left = input[iv::stereo::left];

        static_assert(iv::TypedSamplePortLike<decltype(input)>);
        static_assert(std::same_as<
            typename iv::typed_sample_port_traits<
                std::remove_cvref_t<decltype(input)>>::channel_type,
            iv::stereo>);

        auto const erased_left = static_cast<iv::SamplePortRef>(left);
        input_was_stereo = static_cast<iv::SamplePortRef>(input).channel_type
                == iv::ChannelTypeId::stereo
            && erased_left.channel_type == iv::ChannelTypeId::mono;
        subgraph.outputs("out"_P = input);
    });

    EXPECT_TRUE(input_was_stereo);
}

TEST(Channels, ErasedSampleAndNodeRefsUseRuntimeCheckedChannelMembers)
{
    iv::GraphBuilder graph;
    iv::NodeRef left = iv::details::configure_concrete_node<NamedStereoSource>(graph).node_ref();
    iv::NodeRef right = iv::details::configure_concrete_node<NamedStereoSource>(graph).node_ref();

    auto const named_port = left["main"];
    auto const named_left = named_port[iv::stereo::left];
    auto const default_left = left[iv::stereo::left];
    auto const sum = left + right;
    auto const sum_port = static_cast<iv::SamplePortRef>(sum);
    auto const sum_left = sum[iv::stereo::left];

    EXPECT_EQ(static_cast<iv::SamplePortRef>(named_left).channel_type,
              iv::ChannelTypeId::mono);
    EXPECT_EQ(static_cast<iv::SamplePortRef>(default_left).channel_type,
              iv::ChannelTypeId::mono);
    EXPECT_EQ(sum_port.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(sum_port.channels().size(), 2u);
    EXPECT_EQ(static_cast<iv::SamplePortRef>(sum_left).channel_type,
              iv::ChannelTypeId::mono);

    iv::NodeRef mono = iv::details::configure_concrete_node<MonoPass>(graph).node_ref();
    EXPECT_THROW((void)named_port[iv::mono::center], std::logic_error);
    EXPECT_THROW((void)mono[iv::stereo::left], std::logic_error);
}

TEST(Channels, TiledEventPortsBroadcastInputsAndMergeOutputs)
{
    auto snapshot = tiled_event_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, SourceAnnotationProjectsATiledBundleAsOneStereoVirtualPort)
{
    auto snapshot = annotation_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, IntrospectionKeepsPromotedLayoutOfAnnotatedTiledBundle)
{
    auto snapshot = introspection_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, TypedStreamOperatorsPromoteMonoAndRemainLayoutAgnostic)
{
    auto snapshot = typed_operator_snapshot();
    EXPECT_TRUE(snapshot.ok);

    auto product_snapshot = stereo_scalar_product_snapshot();
    EXPECT_TRUE(product_snapshot.ok);
    EXPECT_EQ(product_snapshot.connection_nodes, 0u);
}

TEST(Channels, ReconstructedNativeChannelSequenceLowersAsWholePort)
{
    auto snapshot = reconstructed_sequence_snapshot(false);
    EXPECT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.connection_nodes, 0u);
}

TEST(Channels, ReorderedNativeChannelsUseOneConnectionNode)
{
    auto snapshot = reconstructed_sequence_snapshot(true);
    EXPECT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.connection_nodes, 1u);
}

TEST(Channels, MonoSourceToATiledInputUsesOneConnectionNode)
{
    auto snapshot = tiled_mono_direct_route_snapshot();
    EXPECT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.connection_nodes, 1u);
}

TEST(Channels, StaticConstantFanoutUsesOneInitializedBufferOwner)
{
    auto snapshot =
        static_constant_fanout_uses_one_initialized_buffer_owner();
    EXPECT_EQ(snapshot.wrapped_node_count, 2u);
    EXPECT_EQ(snapshot.constant_wrapper_count, 0u);
    EXPECT_EQ(snapshot.static_owner_count, 1u);
    EXPECT_EQ(snapshot.static_alias_count, 1u);
    EXPECT_EQ(snapshot.wrapped_node_count, 2u);
    EXPECT_EQ(snapshot.constant_wrapper_count, 0u);
    EXPECT_EQ(snapshot.static_owner_count, 1u);
    EXPECT_EQ(snapshot.static_alias_count, 1u);
}

TEST(Channels, SampleLoweringPlanGroupsConnectionsByTargetPort)
{
    EXPECT_TRUE(sample_lowering_plan_groups_connections_by_target_port());
    EXPECT_TRUE(sample_lowering_plan_groups_connections_by_target_port());
}

TEST(Channels, SampleLoweringPassesHaveExplicitConnectedAndVacantHandOffs)
{
    auto connected = sample_lowering_pass_facts(true);
    auto vacant = sample_lowering_pass_facts(false);
    EXPECT_EQ(connected.planned_groups, 2u);
    EXPECT_EQ(connected.connected_bound_targets, 2u);
    EXPECT_EQ(connected.vacant_bound_targets, 2u);
    EXPECT_EQ(vacant.planned_groups, 1u);
    EXPECT_EQ(vacant.connected_bound_targets, 1u);
    EXPECT_EQ(vacant.vacant_bound_targets, 2u);
    EXPECT_EQ(connected.assigned_subgraph_outputs, 0u);
    EXPECT_EQ(vacant.assigned_subgraph_outputs, 0u);
}

iv::ConfiguredGraphTestView configure_explicit_three_stage()
{
    iv::GraphBuilder builder;
    auto source = iv::details::configure_concrete_node<iv::Constant>(
        builder, iv::Sample{0.25f});
    builder.outputs(source);
    return iv::freeze_configured_graph_for_test(std::move(builder).finish());
}

bool explicit_three_stage_pipeline_builds_a_graph()
{
    auto configured = iv::thaw_configured_graph_for_test(configure_explicit_three_stage());
    auto executable = iv::GraphLowerer::lower(std::move(configured));
    auto compiled = iv::GraphCompiler::compile(std::move(executable));
    return compiled.graph.outputs().size() == 1
        && compiled.introspection.public_sample_outputs.size() == 1;
}

iv::ConfiguredGraphTestView configure_builder_finish()
{
    iv::GraphBuilder builder;
    builder.outputs(iv::details::configure_concrete_node<iv::Constant>(
        builder, iv::Sample{0.5f}));
    return iv::freeze_configured_graph_for_test(std::move(builder).finish());
}

bool builder_build_consumes_the_finished_configuration_value()
{
    auto compiled = compile_graph(
        configure_builder_finish());
    return compiled.graph.outputs().size() == 1
        && compiled.introspection.public_sample_outputs.size() == 1;
}

TEST(Channels, ExplicitConfiguredExecutableAndCompiledStagesBuild)
{
    EXPECT_TRUE(explicit_three_stage_pipeline_builds_a_graph());
    EXPECT_TRUE(builder_build_consumes_the_finished_configuration_value());
}

TEST(Channels, ConnectionLoweringHandlesFanInAndVacantDefaults)
{
    auto fan_in = connection_lowering_snapshot(true);
    auto vacant = connection_lowering_snapshot(false);
    EXPECT_TRUE(fan_in.ok);
    EXPECT_TRUE(vacant.ok);
    EXPECT_EQ(fan_in.connection_nodes, 1u);
    EXPECT_EQ(vacant.connection_nodes, 1u);
}

TEST(Channels, ExecutionRootMaterializesRuntimeSamplePorts)
{
    auto snapshot = execution_root_snapshot();
    EXPECT_TRUE(snapshot.closed_interface);
    EXPECT_TRUE(snapshot.has_runtime_sample_input);
    EXPECT_TRUE(snapshot.has_runtime_sample_output);
}

} // namespace
