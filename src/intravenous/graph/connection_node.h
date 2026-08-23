#pragma once

#include <intravenous/graph/runtime_bindings.h>
#include <intravenous/node/lifecycle.h>

#include <algorithm>
#include <span>
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

// Lowering IR for one concrete destination sample input. Authored sample
// expressions are gathered into ephemeral ports, converted through the
// centralized channel conversion registry, and accumulated into one output.
// A destination channel with no contribution is written from default_value.
class ConnectionNode {
public:
    ConnectionNode(
        std::vector<ConnectionNodeInputConfig> input_configs,
        std::vector<ConnectionNodeEphemeralPortConfig> ephemeral_port_configs,
        OutputConfig output_config,
        Sample default_value,
        std::shared_ptr<RuntimeSampleInputBinding> runtime_binding = {},
        size_t runtime_source_channel_offset = 0)
      : input_configs_(std::move(input_configs)),
        ephemeral_port_configs_(std::move(ephemeral_port_configs)),
        output_config_(std::move(output_config)),
        default_value_(default_value),
        runtime_binding_(std::move(runtime_binding)),
        runtime_source_channel_offset_(runtime_source_channel_offset)
    {
        validate();
        prepare_scratch_layout();
    }

    struct State {
        std::span<Sample> output {};
        std::span<Sample> gathered {};
        std::span<Sample> converted {};
        std::span<Sample> runtime_contribution {};
    };

    [[nodiscard]] std::span<InputConfig const> inputs() const
    {
        return inputs_;
    }

    [[nodiscard]] std::span<OutputConfig const> outputs() const
    {
        return outputs_;
    }

    void declare(DeclarationContext<ConnectionNode> const& ctx) const
    {
        auto const& state = ctx.state();
        ctx.local_array(
            state.output,
            output_channel_count_ * ctx.max_block_size());
        ctx.local_array(
            state.gathered,
            gathered_channel_count_ * ctx.max_block_size());
        ctx.local_array(
            state.converted,
            converted_channel_count_ * ctx.max_block_size());

        // whether the input is statically disconnected from the side panel or not.
        // `runtime_binding != nullptr` can still dynamically resolve to mode `none`.
        if (runtime_binding_)
            ctx.local_array(
                state.runtime_contribution,
                output_channel_count_ * ctx.max_block_size());
    }

    void tick_block(TickBlockContext<ConnectionNode> const& ctx) const
    {
        auto& state = ctx.state();
        auto const runtime_mode = runtime_binding_
            ? runtime_binding_->mode
            : RuntimeSampleInputMode::none;
        auto const sample_count = output_channel_count_ * ctx.block_size;
        auto output = state.output.first(sample_count);

        initialize_output(output, ctx.block_size, runtime_mode);
        gather_inputs(ctx, state.gathered);
        convert_and_accumulate(
            state.gathered, state.converted, output, ctx.block_size);

        if (runtime_mode == RuntimeSampleInputMode::scalar) {
            auto const value = runtime_binding_->value;
            for (auto& sample : output) sample += value;
        } else if (runtime_mode == RuntimeSampleInputMode::timeline) {
            auto runtime_samples = state.runtime_contribution.first(sample_count);
            std::fill(
                runtime_samples.begin(), runtime_samples.end(), Sample{0});
            if (runtime_binding_->read_timeline_block) {
                runtime_binding_->read_timeline_block(
                    runtime_binding_->timeline_lane,
                    runtime_binding_->source_channel,
                    runtime_source_channel_offset_,
                    ctx.index,
                    ctx.block_size,
                    output_config_.channel_layout,
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
        auto const layout = output_config_.channel_layout;
        if (layout.sample_layout == SampleStreamLayout::planar) {
            for (size_t channel = 0; channel < output_channel_count_; ++channel) {
                auto const value = contributed_output_channels_[channel] ||
                        runtime_mode != RuntimeSampleInputMode::none
                    ? Sample{0}
                    : default_value_;
                std::fill_n(
                    output.begin() + channel * frame_count,
                    frame_count,
                    value);
            }
            return;
        }
        for (size_t frame = 0; frame < frame_count; ++frame)
            for (size_t channel = 0; channel < output_channel_count_; ++channel)
                output[frame * output_channel_count_ + channel] =
                    contributed_output_channels_[channel] ||
                            runtime_mode != RuntimeSampleInputMode::none
                        ? Sample{0}
                        : default_value_;
    }

    void gather_inputs(
        TickBlockContext<ConnectionNode> const& ctx,
        std::span<Sample> gathered_storage) const
    {
        auto gathered = gathered_storage.first(
            gathered_channel_count_ * ctx.block_size);
        std::fill(gathered.begin(), gathered.end(), Sample{0});
        for (size_t input = 0; input < input_configs_.size(); ++input) {
            auto const& config = input_configs_[input];
            if (config.input.channel_layout.sample_layout ==
                SampleStreamLayout::planar) {
                for (auto const copy : config.channel_copies) {
                    auto const& ephemeral =
                        ephemeral_port_configs_[copy.ephemeral_port];
                    auto const base = ephemeral_channel_offsets_[
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
                            ephemeral_port_configs_[copy.ephemeral_port];
                        auto const base = ephemeral_channel_offsets_[
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
             ephemeral_i < ephemeral_port_configs_.size();
             ++ephemeral_i) {
            auto const& ephemeral = ephemeral_port_configs_[ephemeral_i];
            auto const converted_channels =
                channel_count(ephemeral.conversion.target);
            auto converted = converted_storage.first(
                converted_channels * frame_count);
            ephemeral.conversion.convert(
                gathered.data() +
                    ephemeral_channel_offsets_[ephemeral_i] * frame_count,
                converted.data(),
                frame_count);

            if (ephemeral.conversion.target.sample_layout ==
                SampleStreamLayout::planar) {
                for (auto const copy : ephemeral.output_channel_copies) {
                    for (size_t frame = 0; frame < frame_count; ++frame) {
                        output[sample_offset(
                            output_config_.channel_layout,
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
                            output_config_.channel_layout,
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
        auto const layout = output_config_.channel_layout;
        if (layout.sample_layout == SampleStreamLayout::planar) {
            for (size_t channel = 0; channel < output_channel_count_; ++channel)
                for (size_t frame = 0; frame < ctx.block_size; ++frame)
                    ctx.outputs[0].write_frame(
                        frame,
                        channel,
                        output[channel * ctx.block_size + frame]);
            return;
        }
        for (size_t frame = 0; frame < ctx.block_size; ++frame)
            for (size_t channel = 0; channel < output_channel_count_; ++channel)
                ctx.outputs[0].write_frame(
                    frame,
                    channel,
                    output[frame * output_channel_count_ + channel]);
    }

    std::vector<ConnectionNodeInputConfig> input_configs_ {};
    std::vector<ConnectionNodeEphemeralPortConfig> ephemeral_port_configs_ {};
    OutputConfig output_config_ {};
    Sample default_value_ = 0.0f;
    std::shared_ptr<RuntimeSampleInputBinding> runtime_binding_ {};
    size_t runtime_source_channel_offset_ = 0;
    std::vector<InputConfig> inputs_ {};
    std::vector<OutputConfig> outputs_ {};
    std::vector<size_t> ephemeral_channel_offsets_ {};
    std::vector<bool> contributed_output_channels_ {};
    size_t output_channel_count_ = 0;
    size_t gathered_channel_count_ = 0;
    size_t converted_channel_count_ = 0;

    void validate()
    {
        inputs_.reserve(input_configs_.size());
        for (auto const& input : input_configs_)
            inputs_.push_back(input.input);
        outputs_.push_back(output_config_);

        std::vector<std::vector<bool>> gathered_channels;
        gathered_channels.reserve(ephemeral_port_configs_.size());
        auto const output_channels = channel_count(output_config_.channel_layout);
        for (auto const& ephemeral : ephemeral_port_configs_) {
            if (!ephemeral.conversion ||
                ephemeral.conversion.source != ephemeral.channel_layout)
                throw std::logic_error(
                    "ConnectionNode ephemeral conversion source layout mismatch");
            gathered_channels.emplace_back(
                channel_count(ephemeral.channel_layout), false);
            auto const converted_channels =
                channel_count(ephemeral.conversion.target);
            for (auto const copy : ephemeral.output_channel_copies) {
                if (copy.converted_channel >= converted_channels ||
                    copy.output_channel >= output_channels)
                    throw std::logic_error(
                        "ConnectionNode output channel copy is out of bounds");
            }
        }

        for (size_t input_i = 0; input_i < input_configs_.size(); ++input_i) {
            auto const input_channels =
                channel_count(input_configs_[input_i].input.channel_layout);
            for (auto const copy : input_configs_[input_i].channel_copies) {
                if (copy.input_channel >= input_channels ||
                    copy.ephemeral_port >= ephemeral_port_configs_.size() ||
                    copy.ephemeral_channel >=
                        gathered_channels[copy.ephemeral_port].size())
                    throw std::logic_error(
                        "ConnectionNode input channel copy is out of bounds");
                if (gathered_channels[copy.ephemeral_port]
                                      [copy.ephemeral_channel])
                    throw std::logic_error(
                        "ConnectionNode ephemeral channel has multiple gathers");
                gathered_channels[copy.ephemeral_port]
                                  [copy.ephemeral_channel] = true;
            }
        }
        for (auto const& channels : gathered_channels)
            if (!std::ranges::all_of(channels, [](bool value) { return value; }))
                throw std::logic_error(
                    "ConnectionNode ephemeral channel has no gathered source");
    }

    void prepare_scratch_layout()
    {
        output_channel_count_ = channel_count(output_config_.channel_layout);
        contributed_output_channels_.assign(output_channel_count_, false);
        ephemeral_channel_offsets_.reserve(ephemeral_port_configs_.size());
        for (auto const& ephemeral : ephemeral_port_configs_) {
            ephemeral_channel_offsets_.push_back(gathered_channel_count_);
            gathered_channel_count_ += channel_count(ephemeral.channel_layout);
            converted_channel_count_ = std::max(
                converted_channel_count_,
                channel_count(ephemeral.conversion.target));
            for (auto const copy : ephemeral.output_channel_copies)
                contributed_output_channels_[copy.output_channel] = true;
        }
    }
};

} // namespace iv
