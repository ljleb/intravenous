#include <intravenous/basic_nodes/routing.h>
#include <intravenous/channel_layout.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/connection_node.hpp>
#include <intravenous/graph/runtime_bindings.h>
#include <intravenous/node/tick.h>
#include <intravenous/runtime/runtime_graph_bindings.h>
#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/runtime/timeline_execution_events.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using iv::operator""_F;

iv::BorrowedSampleBlock runtime_timeline_sample_block {};
size_t connection_conversion_call_count = 0;
size_t connection_conversion_frame_count = 0;

void provide_runtime_timeline_sample_block(
    iv::LaneId,
    iv::TimelineExecutionRealtimeSampleBlockBuilder& builder)
{
    builder.succeed(runtime_timeline_sample_block);
}

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

    constexpr NamedStereoSource() = default;
    constexpr NamedStereoSource(iv::Sample left_, iv::Sample right_)
        : left(left_), right(right_)
    {}

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

struct DefaultMonoPass {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{iv::InputConfig{
            .name = "in",
            .default_value = iv::Sample{0.75f},
        }};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{iv::OutputConfig{.name = "out"}};
    }

    void tick(iv::TickSampleContext<DefaultMonoPass> const& ctx) const
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
            .name = "trigger",
            .type = iv::EventTypeId::trigger,
        }};
    }

    static constexpr auto event_outputs()
    {
        return std::array<iv::EventOutputConfig, 1>{iv::EventOutputConfig{
            .name = "trigger",
            .type = iv::EventTypeId::trigger,
        }};
    }

    void tick(iv::TickSampleContext<EventfulMonoPass> const& ctx) const
    {
        ctx.outputs[0].push(ctx.inputs[0].get());
    }
};

struct ScheduledTriggerSource {
    size_t sample_offset = 0;

    static constexpr auto event_outputs()
    {
        return std::array<iv::EventOutputConfig, 1>{iv::EventOutputConfig{
            .name = "trigger",
            .type = iv::EventTypeId::trigger,
        }};
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

static_assert(!iv::details::has_constexpr_sample_port_configs<NonConstexprPorts>);

template<class Range>
constexpr bool has_generated_type(Range const& types, std::string_view name)
{
    return std::ranges::any_of(types, [name](auto const& type) {
        return std::string_view(type).contains(name);
    });
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

consteval iv::ConnectionNode tracked_connection_node()
{
    constexpr auto mono_planar = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    return iv::details::freeze_connection_node(iv::ConnectionNodeSpec{
        .input_configs = {
            iv::ConnectionNodeInputConfig{
                .input = iv::InputConfig{.channel_layout = mono_planar},
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
        .output_config = iv::OutputConfig{.channel_layout = mono_planar},
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

consteval ChannelTopologySnapshot boundary_adapter_snapshot()
{
    iv::GraphBuilder g;
    (void)g.node<iv::ChannelPack<iv::stereo>>();
    (void)g.node<iv::ChannelUnpack<iv::stereo>>();
    g.outputs();
    auto const built = std::move(g).build();
    return {.ok = built.graph.outputs().empty()};
}

consteval ChannelTopologySnapshot tiled_source_snapshot()
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
    auto const built = std::move(g).build();
    return {
        .ok = built.graph.outputs().size() == 1
            && built.graph.outputs().front().channel_layout.channel_type
                == iv::ChannelTypeId::mono,
    };
}

consteval ChannelTopologySnapshot sample_ref_snapshot()
{
    iv::GraphBuilder g;
    auto source = g.node<NamedStereoSource>(
        iv::Sample{0.25f}, iv::Sample{-0.5f});
    auto stream = source[iv::PortName<"main">{}];
    auto erased = static_cast<iv::SamplePortRef>(stream);
    auto left = static_cast<iv::SamplePortRef>(stream[iv::stereo::left]);

    bool ok = erased.channel_type == iv::ChannelTypeId::stereo
        && erased.channels.size() == 2
        && erased.channels[0].bundle == source.node_bundle_handle()
        && erased.channels[0].port == 0
        && erased.channels[0].channel == 0
        && erased.channels[1].bundle == source.node_bundle_handle()
        && erased.channels[1].port == 0
        && erased.channels[1].channel == 1
        && left.channel_type == iv::ChannelTypeId::mono
        && left.channels.size() == 1
        && left.channels.front() == erased.channels.front();

    g.outputs();
    auto const built = std::move(g).build();
    ok = ok
        && !has_generated_type(
            built.metadata.concrete_node_type_identities, "ChannelUnpack")
        && !has_generated_type(
            built.metadata.concrete_node_type_identities, "ConnectionNode");
    return {.ok = ok};
}

consteval ChannelTopologySnapshot structural_tile_snapshot()
{
    iv::GraphBuilder g;
    auto left = g.node<iv::Constant>(iv::Sample{0.25f});
    auto right = g.node<iv::Constant>(iv::Sample{-0.5f});
    auto tiled = g.tile<iv::stereo>(left, right);
    auto erased = static_cast<iv::SamplePortRef>(tiled);

    bool ok = erased.channel_type == iv::ChannelTypeId::stereo
        && erased.channels.size() == 2
        && erased.channels[0].bundle == left.node_bundle_handle()
        && erased.channels[1].bundle == right.node_bundle_handle();

    g.outputs();
    auto const built = std::move(g).build();
    ok = ok
        && !has_generated_type(
            built.metadata.concrete_node_type_identities, "ChannelPack")
        && !has_generated_type(
            built.metadata.concrete_node_type_identities, "ChannelUnpack")
        && !has_generated_type(
            built.metadata.concrete_node_type_identities, "ConnectionNode");
    return {.ok = ok};
}

consteval ChannelTopologySnapshot qualified_output_snapshot()
{
    iv::GraphBuilder g;
    auto left = g.node<iv::Constant>(iv::Sample{0.25f});
    auto right = g.node<iv::Constant>(iv::Sample{-0.5f});
    g.outputs(
        iv::PortName<"main">{}[iv::stereo::left] =
            static_cast<iv::SamplePortRef>(left),
        iv::PortName<"main">{}[iv::stereo::right] =
            static_cast<iv::SamplePortRef>(right));

    auto after_outputs = g.node<iv::Constant>(iv::Sample{1.0f});
    auto const built = std::move(g).build();
    auto const connection_nodes = std::ranges::count_if(
        built.metadata.concrete_node_type_identities,
        [](auto const& type) {
            return std::string_view(type).contains("ConnectionNode");
        });
    return {
        .ok = after_outputs.node_bundle_handle()
                == right.node_bundle_handle() + 1
            && built.graph.outputs().size() == 1
            && built.graph.outputs().front().channel_layout.channel_type
                == iv::ChannelTypeId::stereo
            && !has_generated_type(
                built.metadata.concrete_node_type_identities, "ChannelPack")
            && !has_generated_type(
                built.metadata.concrete_node_type_identities, "ChannelUnpack"),
        .connection_nodes = static_cast<size_t>(connection_nodes),
    };
}

consteval ChannelTopologySnapshot detach_snapshot()
{
    iv::GraphBuilder g;
    auto left = g.node<iv::Constant>(iv::Sample{0.25f});
    auto right = g.node<iv::Constant>(iv::Sample{-0.5f});
    auto structural = g.tile<iv::stereo>(left, right);
    auto detached = static_cast<iv::SamplePortRef>(structural).detach();
    auto after_detach = g.node<iv::Constant>(iv::Sample{1.0f});
    g.outputs(detached);
    auto const built = std::move(g).build();
    auto const connection_nodes = std::ranges::count_if(
        built.metadata.concrete_node_type_identities,
        [](auto const& type) {
            return std::string_view(type).contains("ConnectionNode");
        });
    return {
        .ok = after_detach.node_bundle_handle()
                == right.node_bundle_handle() + 3
            && !has_generated_type(
                built.metadata.concrete_node_type_identities, "ChannelPack")
            && !has_generated_type(
                built.metadata.concrete_node_type_identities, "ChannelUnpack"),
        .connection_nodes = static_cast<size_t>(connection_nodes),
    };
}

consteval ChannelTopologySnapshot tiled_event_snapshot()
{
    iv::GraphBuilder g;
    auto tiled = iv::_annotate_node_source_info(
        g.node<EventfulMonoPass, iv::stereo>(), "eventful-tiled");
    auto source = g.node<iv::EventConcatenation>(0, iv::EventTypeId::trigger);
    tiled("trigger"_F = source.event_port(0));
    auto merged = tiled.event_port(0);
    auto sink = g.node<iv::EventConcatenation>(1, iv::EventTypeId::trigger);
    sink.connect_event_input(0, merged);
    g.outputs();

    auto const virtual_ports = g.virtual_ports();
    auto const tiled_handle = tiled.node_bundle_handle();
    auto const tiled_event_input_connected = tiled.event_input_is_connected(0);
    auto const built = std::move(g).build();
    auto const merge_count = std::ranges::count_if(
        built.metadata.concrete_node_type_identities,
        [](auto const& type) {
            return std::string_view(type).contains("EventConcatenation");
        });
    return {
        .ok = merged.sources.size() == 1
            && merged.sources.front().bundle == tiled_handle
            && tiled_event_input_connected
            && virtual_ports.event_inputs.size() == 1
            && virtual_ports.event_outputs.size() == 1
            && virtual_ports.event_inputs.front().node_bundle_ports.size() == 1
            && virtual_ports.event_outputs.front().node_bundle_ports.size() == 1
            && virtual_ports.event_inputs.front().config.type
                == iv::EventTypeId::trigger
            && merge_count == 3,
    };
}

consteval ChannelTopologySnapshot annotation_snapshot()
{
    iv::GraphBuilder g;
    auto node = iv::_annotate_node_source_info(
        g.node<MonoPass, iv::stereo>(), "tiled-pass");
    auto const inputs = g.virtual_sample_input_families();
    auto const outputs = g.virtual_sample_output_families();
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

consteval ChannelTopologySnapshot introspection_snapshot()
{
    iv::GraphBuilder g;
    (void)iv::_annotate_node_source_info(
        g.node<MonoPass, iv::stereo>(),
        "tiled-introspection",
        "/tmp/tiled-module.cpp",
        40,
        55);
    auto const metadata = std::move(g).build().introspection;
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

consteval ChannelTopologySnapshot typed_operator_snapshot()
{
    iv::GraphBuilder g;
    auto planar_source = g.node<NamedStereoSource>();
    auto interleaved_source = g.node<NamedInterleavedStereoSource>();
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
            && scaled_erased.channels.size() == 2
            && reverse_scaled_erased.channel_type == iv::ChannelTypeId::stereo
            && reverse_scaled_erased.channels.size() == 2,
    };
}

consteval ChannelTopologySnapshot stereo_scalar_product_snapshot()
{
    iv::GraphBuilder g;
    auto source = g.node<NamedStereoSource>(
        iv::Sample{0.25f}, iv::Sample{-0.5f});
    auto modulation = g.node<MonoPass, iv::stereo>();
    auto stream = source[iv::PortName<"main">{}];
    auto scaled = stream * 0.1f * modulation;
    g.outputs(scaled);
    auto const public_outputs = g.public_sample_output_families();
    auto const built = std::move(g).build();
    if (public_outputs.families.size() != 1) {
        return {.ok = false, .connection_nodes = 1};
    }
    if (public_outputs.families.front().channel_type
        != iv::ChannelTypeId::stereo) {
        return {.ok = false, .connection_nodes = 2};
    }
    if (public_outputs.families.front().channels.size() != 2) {
        return {.ok = false, .connection_nodes = 3};
    }
    if (built.graph.outputs().size() != 1) {
        return {.ok = false, .connection_nodes = 4};
    }
    if (built.graph.outputs().front().channel_layout.channel_type
        != iv::ChannelTypeId::stereo) {
        return {.ok = false, .connection_nodes = 5};
    }
    return {
        .ok = true,
    };
}

consteval ChannelTopologySnapshot reconstructed_sequence_snapshot(bool reverse)
{
    iv::GraphBuilder g;
    auto source = g.node<NamedStereoSource>(
        iv::Sample{0.25f}, iv::Sample{-0.5f});
    auto stream = source[iv::PortName<"main">{}];
    auto erased = static_cast<iv::SamplePortRef>(stream);
    auto reconstructed = iv::SamplePortRef(
        g,
        iv::ChannelTypeId::stereo,
        reverse
            ? std::vector<iv::SampleOutputChannelId>{
                  erased.channels[1], erased.channels[0]}
            : std::vector<iv::SampleOutputChannelId>{
                  erased.channels[0], erased.channels[1]});
    auto pass = g.node<iv::Sum<iv::stereo, iv::SampleStreamLayout::planar, 1>>();
    pass(reconstructed);
    g.outputs(pass);
    auto const built = std::move(g).build();
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

consteval ChannelTopologySnapshot tiled_mono_direct_route_snapshot()
{
    iv::GraphBuilder g;
    auto source = g.node<iv::Constant>(iv::Sample{0.25f});
    auto target = g.node<MonoPass, iv::stereo>();
    target(source);
    g.outputs(target);
    auto const built = std::move(g).build();
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

consteval StaticConstantFanoutSnapshot
static_constant_fanout_uses_one_initialized_buffer_owner()
{
    iv::GraphBuilder g;
    auto source = g.node<iv::Constant>(iv::Sample{0.25f});
    auto first = g.node<MonoPass>();
    auto second = g.node<MonoPass>();
    first(source);
    second(source);
    g.outputs(
        iv::PortName<"first">{} = first,
        iv::PortName<"second">{} = second);

    auto const built = std::move(g).build();
    StaticConstantFanoutSnapshot result {};
    if (has_generated_type(
            built.metadata.concrete_node_type_identities, "Broadcast")) {
        return result;
    }
    for (auto const& scc : built.graph._scc_wrappers) {
        result.wrapped_node_count += scc._nodes.size;
        for (size_t node_i = 0; node_i < scc._nodes.size; ++node_i) {
            auto const& node = scc._nodes[node_i];
            if (scc._global_node_indices[node_i] == 0) {
                ++result.constant_wrapper_count;
            }
            if (node._input_port_data_nodes.size == 1
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

consteval bool sample_lowering_plan_groups_connections_by_target_port()
{
    iv::GraphBuilderConnections connections;
    connections.record_authored_sample_connection({
        .source_type = iv::ChannelTypeId::mono,
        .source_channels = {{.bundle = 1, .port = 0, .channel = 0}},
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {{.bundle = 4, .port = 2, .channel = 0}},
    });
    connections.record_authored_sample_connection({
        .source_type = iv::ChannelTypeId::mono,
        .source_channels = {{.bundle = 2, .port = 1, .channel = 0}},
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {{.bundle = 4, .port = 2, .channel = 0}},
    });
    connections.record_authored_sample_connection({
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

consteval ChannelTopologySnapshot connection_lowering_snapshot(bool connected)
{
    iv::GraphBuilder g;
    auto pass = g.node<DefaultMonoPass>();
    if (connected) {
        pass(g.node<iv::Constant>(iv::Sample{0.25f}));
        pass(g.node<iv::Constant>(iv::Sample{-0.5f}));
    }
    g.outputs(pass);
    auto const built = std::move(g).build();
    return {
        .ok = true,
        .connection_nodes = static_cast<size_t>(std::ranges::count_if(
            built.metadata.concrete_node_type_identities,
            [](auto const& type) {
                return std::string_view(type).contains("ConnectionNode");
            })),
    };
}

consteval iv::details::SampleLoweringPassFacts
sample_lowering_pass_facts(bool connected)
{
    iv::GraphBuilder g;
    auto pass = g.node<DefaultMonoPass>();
    if (connected)
        pass(g.node<iv::Constant>(iv::Sample{0.25f}));
    g.outputs(pass);
    return iv::details::GraphLowererTestAccess::sample_lowering_pass_facts(
        g.finish());
}

struct ExecutionRootSnapshot {
    bool closed_interface = false;
    bool has_runtime_sample_input = false;
    bool has_runtime_sample_output = false;
};

consteval ExecutionRootSnapshot execution_root_snapshot()
{
    iv::GraphBuilder g;
    auto input = g.input<"in">(iv::Sample{-1.0f});
    auto pass = g.node<MonoPass>();
    pass(input);
    g.outputs(iv::PortName<"main">{} = pass);
    auto const built = std::move(g).build({.execution_root = true});
    return {
        .closed_interface = built.graph.inputs().empty()
            && built.graph.outputs().empty()
            && built.graph.event_inputs().empty()
            && built.graph.event_outputs().empty(),
        .has_runtime_sample_input = has_generated_type(
            built.metadata.concrete_node_type_identities,
            "RuntimeSampleInputNode"),
        .has_runtime_sample_output = has_generated_type(
            built.metadata.concrete_node_type_identities,
            "RuntimeSampleOutput"),
    };
}

} // namespace

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

TEST(Channels, RuntimeTimelineSampleReaderWritesRequestedInterleavedLayout)
{
    constexpr auto planar = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    constexpr auto interleaved = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };
    std::array<iv::Sample, 6> source{
        iv::Sample{1}, iv::Sample{2}, iv::Sample{3},
        iv::Sample{10}, iv::Sample{20}, iv::Sample{30},
    };
    std::array<iv::Sample, 6> target{};
    runtime_timeline_sample_block = borrowed_block(source, planar, 3);
    auto const previous_subscriber =
        iv::iv_runtime_timeline_execution_realtime_sample_block_requested_event;
    iv::iv_runtime_timeline_execution_realtime_sample_block_requested_event =
        &provide_runtime_timeline_sample_block;
    auto bindings = iv::make_graph_runtime_bindings();
    auto binding = bindings->sample_input("interleaved-reader-regression");
    binding->read_timeline_block(
        iv::LaneId{1}, 0, 0, 0, 3, interleaved, target);
    iv::iv_runtime_timeline_execution_realtime_sample_block_requested_event =
        previous_subscriber;
    EXPECT_EQ(target, (std::array<iv::Sample, 6>{
        iv::Sample{1}, iv::Sample{10}, iv::Sample{2},
        iv::Sample{20}, iv::Sample{3}, iv::Sample{30},
    }));
}

TEST(Channels, ConnectionNodeConvertsEachEphemeralExpressionAsOneBlock)
{
    static constexpr auto connection = tracked_connection_node();
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
    constexpr auto snapshot = boundary_adapter_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, FullyMonoNodeTilesIntoAStaticStereoOutput)
{
    constexpr auto snapshot = tiled_source_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, SampleRefsExposeOrderedStructuralChannelIdentity)
{
    constexpr auto snapshot = sample_ref_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, GraphBuilderTileIsPureStructuralComposition)
{
    constexpr auto snapshot = structural_tile_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, ChannelQualifiedPublicOutputsAreProjectedOnlyAtCompletion)
{
    constexpr auto snapshot = qualified_output_snapshot();
    EXPECT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.connection_nodes, 1u);
}

TEST(Channels, DetachAuthorsOnlyItsExplicitWriterAndReaderNodes)
{
    constexpr auto snapshot = detach_snapshot();
    EXPECT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.connection_nodes, 1u);
}

TEST(Channels, TiledEventPortsBroadcastInputsAndMergeOutputs)
{
    constexpr auto snapshot = tiled_event_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, SourceAnnotationProjectsATiledBundleAsOneStereoVirtualPort)
{
    constexpr auto snapshot = annotation_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, IntrospectionKeepsPromotedLayoutOfAnnotatedTiledBundle)
{
    constexpr auto snapshot = introspection_snapshot();
    EXPECT_TRUE(snapshot.ok);
}

TEST(Channels, TypedStreamOperatorsPromoteMonoAndRemainLayoutAgnostic)
{
    constexpr auto snapshot = typed_operator_snapshot();
    EXPECT_TRUE(snapshot.ok);

    constexpr auto product_snapshot = stereo_scalar_product_snapshot();
    EXPECT_TRUE(product_snapshot.ok);
    EXPECT_EQ(product_snapshot.connection_nodes, 0u);
}

TEST(Channels, ReconstructedNativeChannelSequenceLowersAsWholePort)
{
    constexpr auto snapshot = reconstructed_sequence_snapshot(false);
    EXPECT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.connection_nodes, 0u);
}

TEST(Channels, ReorderedNativeChannelsUseOneConnectionNode)
{
    constexpr auto snapshot = reconstructed_sequence_snapshot(true);
    EXPECT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.connection_nodes, 1u);
}

TEST(Channels, MonoSourceToATiledInputUsesOneConnectionNode)
{
    constexpr auto snapshot = tiled_mono_direct_route_snapshot();
    EXPECT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.connection_nodes, 1u);
}

TEST(Channels, StaticConstantFanoutUsesOneInitializedBufferOwner)
{
    constexpr auto snapshot =
        static_constant_fanout_uses_one_initialized_buffer_owner();
    static_assert(snapshot.wrapped_node_count == 2);
    static_assert(snapshot.constant_wrapper_count == 0);
    static_assert(snapshot.static_owner_count == 1);
    static_assert(snapshot.static_alias_count == 1);
    EXPECT_EQ(snapshot.wrapped_node_count, 2u);
    EXPECT_EQ(snapshot.constant_wrapper_count, 0u);
    EXPECT_EQ(snapshot.static_owner_count, 1u);
    EXPECT_EQ(snapshot.static_alias_count, 1u);
}

TEST(Channels, SampleLoweringPlanGroupsConnectionsByTargetPort)
{
    static_assert(sample_lowering_plan_groups_connections_by_target_port());
    EXPECT_TRUE(sample_lowering_plan_groups_connections_by_target_port());
}

TEST(Channels, SampleLoweringPassesHaveExplicitConnectedAndVacantHandOffs)
{
    constexpr auto connected = sample_lowering_pass_facts(true);
    constexpr auto vacant = sample_lowering_pass_facts(false);
    static_assert(connected.planned_groups == 2);
    static_assert(connected.connected_bound_targets == 2);
    static_assert(connected.vacant_bound_targets == 2);
    static_assert(vacant.planned_groups == 1);
    static_assert(vacant.connected_bound_targets == 1);
    static_assert(vacant.vacant_bound_targets == 2);
    EXPECT_EQ(connected.assigned_subgraph_outputs, 0u);
    EXPECT_EQ(vacant.assigned_subgraph_outputs, 0u);
}

consteval bool explicit_three_stage_pipeline_builds_a_graph()
{
    iv::GraphBuilder builder;
    auto source = builder.node<iv::Constant>(iv::Sample{0.25f});
    builder.outputs(source);

    auto authored = builder.finish();
    auto executable = iv::GraphLowerer::lower(authored);
    auto compiled = iv::GraphCompiler::compile(std::move(executable));
    return compiled.graph.outputs().size() == 1
        && compiled.introspection.public_sample_outputs.size() == 1;
}

consteval bool builder_build_consumes_the_finished_authoring_value()
{
    iv::GraphBuilder builder;
    builder.outputs(builder.node<iv::Constant>(iv::Sample{0.5f}));
    auto compiled = std::move(builder).build();
    return compiled.graph.outputs().size() == 1
        && compiled.introspection.public_sample_outputs.size() == 1;
}

TEST(Channels, ExplicitAuthoredExecutableAndCompiledStagesBuild)
{
    static_assert(explicit_three_stage_pipeline_builds_a_graph());
    static_assert(builder_build_consumes_the_finished_authoring_value());
}

TEST(Channels, ConnectionLoweringHandlesFanInAndVacantDefaults)
{
    constexpr auto fan_in = connection_lowering_snapshot(true);
    constexpr auto vacant = connection_lowering_snapshot(false);
    EXPECT_TRUE(fan_in.ok);
    EXPECT_TRUE(vacant.ok);
    EXPECT_EQ(fan_in.connection_nodes, 1u);
    EXPECT_EQ(vacant.connection_nodes, 1u);
}

TEST(Channels, ExecutionRootMaterializesRuntimeSamplePorts)
{
    constexpr auto snapshot = execution_root_snapshot();
    EXPECT_TRUE(snapshot.closed_interface);
    EXPECT_TRUE(snapshot.has_runtime_sample_input);
    EXPECT_TRUE(snapshot.has_runtime_sample_output);
}
