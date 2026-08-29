#pragma once

#include <intravenous/graph/runtime_bindings.h>
#include <intravenous/graph/static_storage.hpp>
#include <intravenous/node/lifecycle.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iv {
struct ConnectionNodeInputChannelCopy {
    size_t input_channel = 0;
    size_t ephemeral_port = 0;
    size_t ephemeral_channel = 0;

    bool operator==(ConnectionNodeInputChannelCopy const&) const = default;
};

struct ConnectionNodeInputConfig {
    InputConfig input {};
    std::vector<ConnectionNodeInputChannelCopy> channel_copies {};
};

struct ConnectionNodeOutputChannelCopy {
    size_t converted_channel = 0;
    size_t output_channel = 0;

    bool operator==(ConnectionNodeOutputChannelCopy const&) const = default;
};

struct ConnectionNodeEphemeralPortConfig {
    ChannelLayout channel_layout {};
    ChannelConversionPlan conversion {};
    std::vector<ConnectionNodeOutputChannelCopy> output_channel_copies {};
};

struct StaticConnectionNodeInputConfig {
    StaticString name {};
    ChannelLayout channel_layout {
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };
    size_t history = 0;
    Sample default_value = 0.0;
    Sample min = -std::numeric_limits<Sample::storage>::infinity();
    Sample max = std::numeric_limits<Sample::storage>::infinity();
    StaticSpan<ConnectionNodeInputChannelCopy> channel_copies {};

    constexpr InputConfig config() const
    {
        return {
            .name = std::string(name.view()),
            .channel_layout = channel_layout,
            .history = history,
            .default_value = default_value,
            .min = min,
            .max = max,
        };
    }
};

struct StaticConnectionNodeOutputConfig {
    StaticString name {};
    ChannelLayout channel_layout {
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };
    size_t latency = 0;
    size_t history = 0;

    constexpr OutputConfig config() const
    {
        return {
            .name = std::string(name.view()),
            .channel_layout = channel_layout,
            .latency = latency,
            .history = history,
        };
    }
};

struct StaticConnectionNodeEphemeralPortConfig {
    ChannelLayout channel_layout {};
    ChannelConversionPlan conversion {};
    StaticSpan<ConnectionNodeOutputChannelCopy> output_channel_copies {};
};

struct ConnectionNodeSpec {
    std::vector<ConnectionNodeInputConfig> input_configs {};
    std::vector<ConnectionNodeEphemeralPortConfig> ephemeral_port_configs {};
    OutputConfig output_config {};
    Sample default_value = 0.0f;
    std::string runtime_binding_id {};
    size_t runtime_source_channel_offset = 0;
};

struct ConnectionNode {
public:
    StaticSpan<StaticConnectionNodeInputConfig> input_configs {};
    StaticSpan<StaticConnectionNodeEphemeralPortConfig>
        ephemeral_port_configs {};
    StaticConnectionNodeOutputConfig output_config {};
    Sample default_value = 0.0f;
    StaticString runtime_binding_id {};
    size_t runtime_source_channel_offset = 0;
    StaticSpan<size_t> ephemeral_channel_offsets {};
    StaticSpan<std::uint8_t> contributed_output_channels {};
    size_t output_channel_count = 0;
    size_t gathered_channel_count = 0;
    size_t converted_channel_count = 0;

    struct State {
        std::span<Sample> output {};
        std::span<Sample> gathered {};
        std::span<Sample> converted {};
        std::span<Sample> runtime_contribution {};
        std::span<RuntimeSampleInputBinding const*> runtime_binding {};
    };

    [[nodiscard]] constexpr std::vector<InputConfig> inputs() const
    {
        std::vector<InputConfig> result;
        result.reserve(input_configs.size);
        for (auto const& input : input_configs)
            result.push_back(input.config());
        return result;
    }

    [[nodiscard]] constexpr auto outputs() const
    {
        return std::array<OutputConfig, 1>{ output_config.config() };
    }

    void declare(DeclarationContext<ConnectionNode> const& ctx) const
    {
        auto const& state = ctx.state();
        ctx.local_array(
            state.output,
            output_channel_count * ctx.max_block_size());
        ctx.local_array(
            state.gathered,
            gathered_channel_count * ctx.max_block_size());
        ctx.local_array(
            state.converted,
            converted_channel_count * ctx.max_block_size());
        if (!runtime_binding_id.empty()) {
            ctx.local_array(
                state.runtime_contribution,
                output_channel_count * ctx.max_block_size());
            ctx.local_array(state.runtime_binding, 1);
        }
    }

    void initialize(InitializationContext<ConnectionNode> const& ctx) const
    {
        if (!runtime_binding_id.empty())
            ctx.state().runtime_binding.front() =
                ctx.resources.runtime_bindings.sample_input(
                    runtime_binding_id.view());
    }

    void tick_block(TickBlockContext<ConnectionNode> const& ctx) const
    {
        auto& state = ctx.state();
        auto const* runtime_binding = state.runtime_binding.empty()
            ? nullptr
            : state.runtime_binding.front();
        auto const runtime_mode = runtime_binding
            ? runtime_binding->mode
            : RuntimeSampleInputMode::none;
        auto const sample_count = output_channel_count * ctx.block_size;
        auto output = state.output.first(sample_count);

        initialize_output(output, ctx.block_size, runtime_mode);
        gather_inputs(ctx, state.gathered);
        convert_and_accumulate(
            state.gathered, state.converted, output, ctx.block_size);

        if (runtime_mode == RuntimeSampleInputMode::scalar) {
            auto const value = runtime_binding->value;
            for (auto& sample : output) sample += value;
        } else if (runtime_mode == RuntimeSampleInputMode::timeline) {
            auto runtime_samples = state.runtime_contribution.first(sample_count);
            std::fill(
                runtime_samples.begin(), runtime_samples.end(), Sample{0});
            if (runtime_binding->read_timeline_block) {
                runtime_binding->read_timeline_block(
                    runtime_binding->timeline_lane,
                    runtime_binding->source_channel,
                    runtime_source_channel_offset,
                    ctx.index,
                    ctx.block_size,
                    output_config.channel_layout,
                    runtime_samples);
            }
            for (size_t sample = 0; sample < sample_count; ++sample)
                output[sample] += runtime_samples[sample];
        }

        write_output(ctx, output);
    }

private:
    static size_t sample_offset(
        ChannelLayout layout,
        size_t frame,
        size_t channel,
        size_t frame_count)
    {
        return layout.sample_layout == SampleStreamLayout::planar
            ? channel * frame_count + frame
            : frame * channel_count(layout) + channel;
    }

    void initialize_output(
        std::span<Sample> output,
        size_t frame_count,
        RuntimeSampleInputMode runtime_mode) const
    {
        auto const layout = output_config.channel_layout;
        if (layout.sample_layout == SampleStreamLayout::planar) {
            for (size_t channel = 0; channel < output_channel_count; ++channel) {
                auto const value = contributed_output_channels[channel] ||
                        runtime_mode != RuntimeSampleInputMode::none
                    ? Sample{0}
                    : default_value;
                std::fill_n(
                    output.begin() + channel * frame_count,
                    frame_count,
                    value);
            }
            return;
        }
        for (size_t frame = 0; frame < frame_count; ++frame)
            for (size_t channel = 0; channel < output_channel_count; ++channel)
                output[frame * output_channel_count + channel] =
                    contributed_output_channels[channel] ||
                            runtime_mode != RuntimeSampleInputMode::none
                        ? Sample{0}
                        : default_value;
    }

    void gather_inputs(
        TickBlockContext<ConnectionNode> const& ctx,
        std::span<Sample> gathered_storage) const
    {
        auto gathered = gathered_storage.first(
            gathered_channel_count * ctx.block_size);
        std::fill(gathered.begin(), gathered.end(), Sample{0});
        for (size_t input = 0; input < input_configs.size; ++input) {
            auto const& config = input_configs[input];
            if (config.channel_layout.sample_layout ==
                SampleStreamLayout::planar) {
                for (auto const copy : config.channel_copies) {
                    auto const& ephemeral =
                        ephemeral_port_configs[copy.ephemeral_port];
                    auto const base = ephemeral_channel_offsets[
                        copy.ephemeral_port] * ctx.block_size;
                    for (size_t frame = 0; frame < ctx.block_size; ++frame) {
                        gathered[base + sample_offset(
                            ephemeral.channel_layout,
                            frame,
                            copy.ephemeral_channel,
                            ctx.block_size)] =
                            ctx.inputs[input].get_frame(
                                frame, copy.input_channel);
                    }
                }
            } else {
                for (size_t frame = 0; frame < ctx.block_size; ++frame) {
                    for (auto const copy : config.channel_copies) {
                        auto const& ephemeral =
                            ephemeral_port_configs[copy.ephemeral_port];
                        auto const base = ephemeral_channel_offsets[
                            copy.ephemeral_port] * ctx.block_size;
                        gathered[base + sample_offset(
                            ephemeral.channel_layout,
                            frame,
                            copy.ephemeral_channel,
                            ctx.block_size)] =
                            ctx.inputs[input].get_frame(
                                frame, copy.input_channel);
                    }
                }
            }
        }
    }

    void convert_and_accumulate(
        std::span<Sample const> gathered,
        std::span<Sample> converted_storage,
        std::span<Sample> output,
        size_t frame_count) const
    {
        for (size_t ephemeral_i = 0;
             ephemeral_i < ephemeral_port_configs.size;
             ++ephemeral_i) {
            auto const& ephemeral = ephemeral_port_configs[ephemeral_i];
            auto const converted_channels =
                channel_count(ephemeral.conversion.target);
            auto converted = converted_storage.first(
                converted_channels * frame_count);
            ephemeral.conversion.convert(
                gathered.data() +
                    ephemeral_channel_offsets[ephemeral_i] * frame_count,
                converted.data(),
                frame_count);

            if (ephemeral.conversion.target.sample_layout ==
                SampleStreamLayout::planar) {
                for (auto const copy : ephemeral.output_channel_copies) {
                    for (size_t frame = 0; frame < frame_count; ++frame) {
                        output[sample_offset(
                            output_config.channel_layout,
                            frame,
                            copy.output_channel,
                            frame_count)] +=
                            converted[copy.converted_channel * frame_count + frame];
                    }
                }
            } else {
                for (size_t frame = 0; frame < frame_count; ++frame) {
                    for (auto const copy : ephemeral.output_channel_copies) {
                        output[sample_offset(
                            output_config.channel_layout,
                            frame,
                            copy.output_channel,
                            frame_count)] +=
                            converted[frame * converted_channels +
                                      copy.converted_channel];
                    }
                }
            }
        }
    }

    void write_output(
        TickBlockContext<ConnectionNode> const& ctx,
        std::span<Sample const> output) const
    {
        auto const layout = output_config.channel_layout;
        if (layout.sample_layout == SampleStreamLayout::planar) {
            for (size_t channel = 0; channel < output_channel_count; ++channel)
                for (size_t frame = 0; frame < ctx.block_size; ++frame)
                    ctx.outputs[0].write_frame(
                        frame,
                        channel,
                        output[channel * ctx.block_size + frame]);
            return;
        }
        for (size_t frame = 0; frame < ctx.block_size; ++frame)
            for (size_t channel = 0; channel < output_channel_count; ++channel)
                ctx.outputs[0].write_frame(
                    frame,
                    channel,
                    output[frame * output_channel_count + channel]);
    }
};

namespace details {
consteval void validate_connection_node_spec(ConnectionNodeSpec const& spec)
{
    auto const output_channel_count =
        channel_count(spec.output_config.channel_layout);
    std::vector<std::vector<bool>> gathered_channels;
    gathered_channels.reserve(spec.ephemeral_port_configs.size());
    for (auto const& ephemeral : spec.ephemeral_port_configs) {
        if (!ephemeral.conversion
            || ephemeral.conversion.source != ephemeral.channel_layout) {
            throw std::logic_error(
                "ConnectionNode ephemeral conversion source layout mismatch");
        }
        gathered_channels.emplace_back(
            channel_count(ephemeral.channel_layout), false);
        auto const converted_channels =
            channel_count(ephemeral.conversion.target);
        for (auto const copy : ephemeral.output_channel_copies) {
            if (copy.converted_channel >= converted_channels
                || copy.output_channel >= output_channel_count) {
                throw std::logic_error(
                    "ConnectionNode output channel copy is out of bounds");
            }
        }
    }

    for (auto const& input : spec.input_configs) {
        auto const input_channels = channel_count(input.input.channel_layout);
        for (auto const copy : input.channel_copies) {
            if (copy.input_channel >= input_channels
                || copy.ephemeral_port >= spec.ephemeral_port_configs.size()
                || copy.ephemeral_channel
                    >= gathered_channels[copy.ephemeral_port].size()) {
                throw std::logic_error(
                    "ConnectionNode input channel copy is out of bounds");
            }
            if (gathered_channels[copy.ephemeral_port]
                                   [copy.ephemeral_channel]) {
                throw std::logic_error(
                    "ConnectionNode ephemeral channel has multiple gathers");
            }
            gathered_channels[copy.ephemeral_port]
                               [copy.ephemeral_channel] = true;
        }
    }
    for (auto const& channels : gathered_channels) {
        if (!std::ranges::all_of(channels, [](bool value) { return value; })) {
            throw std::logic_error(
                "ConnectionNode ephemeral channel has no gathered source");
        }
    }
}

consteval ConnectionNode freeze_connection_node(ConnectionNodeSpec const& spec)
{
    validate_connection_node_spec(spec);
    auto const output_channel_count =
        channel_count(spec.output_config.channel_layout);
    std::vector<StaticConnectionNodeInputConfig> input_configs;
    input_configs.reserve(spec.input_configs.size());
    for (auto const& input : spec.input_configs) {
        input_configs.push_back({
            .name = define_static_string(input.input.name),
            .channel_layout = input.input.channel_layout,
            .history = input.input.history,
            .default_value = input.input.default_value,
            .min = input.input.min,
            .max = input.input.max,
            .channel_copies = define_static_span(input.channel_copies),
        });
    }

    std::vector<StaticConnectionNodeEphemeralPortConfig> ephemeral_configs;
    ephemeral_configs.reserve(spec.ephemeral_port_configs.size());
    std::vector<size_t> ephemeral_channel_offsets;
    ephemeral_channel_offsets.reserve(spec.ephemeral_port_configs.size());
    std::vector<std::uint8_t> contributed_output_channels(
        output_channel_count, 0);
    size_t gathered_channel_count = 0;
    size_t converted_channel_count = 0;
    for (auto const& ephemeral : spec.ephemeral_port_configs) {
        ephemeral_configs.push_back({
            .channel_layout = ephemeral.channel_layout,
            .conversion = ephemeral.conversion,
            .output_channel_copies =
                define_static_span(ephemeral.output_channel_copies),
        });
        ephemeral_channel_offsets.push_back(gathered_channel_count);
        gathered_channel_count += channel_count(ephemeral.channel_layout);
        converted_channel_count = std::max(
            converted_channel_count,
            channel_count(ephemeral.conversion.target));
        for (auto const copy : ephemeral.output_channel_copies)
            contributed_output_channels[copy.output_channel] = 1;
    }

    return {
        .input_configs = define_static_span(input_configs),
        .ephemeral_port_configs = define_static_span(ephemeral_configs),
        .output_config = {
            .name = define_static_string(spec.output_config.name),
            .channel_layout = spec.output_config.channel_layout,
            .latency = spec.output_config.latency,
            .history = spec.output_config.history,
        },
        .default_value = spec.default_value,
        .runtime_binding_id = define_static_string(spec.runtime_binding_id),
        .runtime_source_channel_offset = spec.runtime_source_channel_offset,
        .ephemeral_channel_offsets =
            define_static_span(ephemeral_channel_offsets),
        .contributed_output_channels =
            define_static_span(contributed_output_channels),
        .output_channel_count = output_channel_count,
        .gathered_channel_count = gathered_channel_count,
        .converted_channel_count = converted_channel_count,
    };
}

consteval ConnectionNode freeze_generated_node(ConnectionNodeSpec const& spec)
{
    return freeze_connection_node(spec);
}
} // namespace details
} // namespace iv
