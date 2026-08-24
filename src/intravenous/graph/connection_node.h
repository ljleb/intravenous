#pragma once

#include <intravenous/graph/runtime_bindings.h>
#include <intravenous/graph/static_storage.h>
#include <intravenous/node/lifecycle.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <stdexcept>
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

// Lowering IR for one concrete destination sample input. Authored sample
// expressions are gathered into ephemeral ports, converted through the
// centralized channel conversion registry, and accumulated into one output.
// A destination channel with no contribution is written from default_value.
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
        std::span<RuntimeSampleInputBinding> runtime_binding {};
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

        // whether the input is statically disconnected from the side panel or not.
        // `runtime_binding != nullptr` can still dynamically resolve to mode `none`.
        if (!runtime_binding_id.empty()) {
            ctx.local_array(
                state.runtime_contribution,
                output_channel_count * ctx.max_block_size());
            ctx.local_array(state.runtime_binding, 1);
            ctx.export_array(
                std::string(runtime_binding_id.view()),
                state.runtime_binding);
        }
    }

    void tick_block(TickBlockContext<ConnectionNode> const& ctx) const
    {
        auto& state = ctx.state();
        auto const* runtime_binding = state.runtime_binding.empty()
            ? nullptr
            : &state.runtime_binding.front();
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

} // namespace iv
