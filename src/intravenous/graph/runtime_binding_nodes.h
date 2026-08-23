#pragma once

#include <intravenous/graph/runtime_bindings.h>
#include <intravenous/node/lifecycle.h>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {

class RuntimeSampleInputNode {
public:
    struct State {
        std::span<Sample> samples {};
    };

    RuntimeSampleInputNode(
        OutputConfig output,
        Sample default_value,
        std::shared_ptr<RuntimeSampleInputBinding> binding)
      : output_(std::move(output)),
        default_value_(default_value),
        binding_(std::move(binding))
    {}

    auto outputs() const { return std::array<OutputConfig, 1>{output_}; }

    void declare(DeclarationContext<RuntimeSampleInputNode> const& ctx) const
    {
        ctx.local_array(
            ctx.state().samples,
            channel_count(output_.channel_layout) * ctx.max_block_size());
    }

    void tick_block(TickBlockContext<RuntimeSampleInputNode> const& ctx) const
    {
        auto const channels = channel_count(output_.channel_layout);
        auto samples = ctx.state().samples.first(channels * ctx.block_size);
        auto const mode = binding_
            ? binding_->mode
            : RuntimeSampleInputMode::none;
        if (mode == RuntimeSampleInputMode::timeline) {
            std::fill(samples.begin(), samples.end(), Sample{0.0f});
            if (binding_->read_timeline_block) {
                binding_->read_timeline_block(
                    binding_->timeline_lane,
                    binding_->source_channel,
                    0,
                    ctx.index,
                    ctx.block_size,
                    output_.channel_layout,
                    samples);
            }
        } else {
            auto const value = mode == RuntimeSampleInputMode::scalar
                ? binding_->value
                : default_value_;
            std::fill(samples.begin(), samples.end(), value);
        }

        for (size_t frame = 0; frame < ctx.block_size; ++frame)
            for (size_t channel = 0; channel < channels; ++channel)
                ctx.outputs[0].write_frame(
                    frame, channel, samples[channel * ctx.block_size + frame]);
    }

private:
    OutputConfig output_ {};
    Sample default_value_ = 0.0f;
    std::shared_ptr<RuntimeSampleInputBinding> binding_ {};
};

class RuntimeEventInputNode {
public:
    RuntimeEventInputNode(
        EventTypeId type,
        std::shared_ptr<RuntimeEventInputBinding> binding)
      : type_(type), binding_(std::move(binding))
    {}

    auto event_outputs() const
    {
        return std::array<EventOutputConfig, 1>{{EventOutputConfig{.type = type_}}};
    }

    void tick_block(TickBlockContext<RuntimeEventInputNode> const& ctx) const
    {
        if (!binding_ || !binding_->read_timeline_block) return;
        auto const timeline_lane = binding_->timeline_lane;
        if (!timeline_lane) return;
        auto const view = binding_->read_timeline_block(
            timeline_lane, ctx.index, ctx.block_size);
        for (size_t i = 0; i < view.size; ++i)
            ctx.event_outputs[0].push(view.data[i]);
    }

private:
    EventTypeId type_ = EventTypeId::empty;
    std::shared_ptr<RuntimeEventInputBinding> binding_ {};
};

class RuntimeSampleOutputNode {
public:
    struct State {
        std::span<Sample> samples {};
    };

    RuntimeSampleOutputNode(
        InputConfig input,
        std::shared_ptr<RuntimeOutputBinding> binding)
      : input_(std::move(input)), binding_(std::move(binding))
    {}

    auto inputs() const { return std::array<InputConfig, 1>{input_}; }

    void declare(DeclarationContext<RuntimeSampleOutputNode> const& ctx) const
    {
        ctx.local_array(
            ctx.state().samples,
            channel_count(input_.channel_layout) * ctx.max_block_size());
    }

    void tick_block(TickBlockContext<RuntimeSampleOutputNode> const& ctx) const
    {
        if (!binding_ || !binding_->publish_sample_block) return;
        auto const target_lane = binding_->target_lane;
        if (!target_lane) return;
        auto const channels = channel_count(input_.channel_layout);
        auto samples = ctx.state().samples.first(channels * ctx.block_size);
        for (size_t channel = 0; channel < channels; ++channel)
            for (size_t frame = 0; frame < ctx.block_size; ++frame)
                samples[channel * ctx.block_size + frame] =
                    ctx.inputs[0].get_frame(frame, channel);
        binding_->publish_sample_block(
            target_lane, samples,
            ChannelLayout{
                .channel_type = input_.channel_layout.channel_type,
                .sample_layout = SampleStreamLayout::planar,
            },
            ctx.block_size);
    }

private:
    InputConfig input_ {};
    std::shared_ptr<RuntimeOutputBinding> binding_ {};
};

class RuntimeEventOutputNode {
public:
    struct State {
        std::span<TimedEvent> events {};
    };

    RuntimeEventOutputNode(
        EventTypeId type,
        std::shared_ptr<RuntimeOutputBinding> binding)
      : type_(type), binding_(std::move(binding))
    {}

    auto event_inputs() const
    {
        return std::array<EventInputConfig, 1>{{EventInputConfig{.type = type_}}};
    }

    void declare(DeclarationContext<RuntimeEventOutputNode> const& ctx) const
    {
        ctx.local_array(
            ctx.state().events,
            calculate_event_port_buffer_capacity(
                ctx.event_port_buffer_base_multiplier(), type_));
    }

    void tick_block(TickBlockContext<RuntimeEventOutputNode> const& ctx) const
    {
        if (!binding_ || !binding_->publish_event_block) return;
        auto const target_lane = binding_->target_lane;
        if (!target_lane) return;
        auto const input =
            ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
        auto const count = std::min(input.size(), ctx.state().events.size());
        for (size_t i = 0; i < count; ++i)
            ctx.state().events[i] = input[i];
        binding_->publish_event_block(
            target_lane, ctx.state().events.first(count));
    }

private:
    EventTypeId type_ = EventTypeId::empty;
    std::shared_ptr<RuntimeOutputBinding> binding_ {};
};

class RuntimeSampleOutputFamilyNode {
public:
    struct State {
        std::span<Sample> aggregate_samples {};
        std::span<Sample> member_samples {};
    };

    RuntimeSampleOutputFamilyNode(
        std::vector<InputConfig> inputs,
        std::vector<std::shared_ptr<RuntimeOutputBinding>> member_bindings,
        std::shared_ptr<RuntimeOutputBinding> aggregate_binding)
      : inputs_(std::move(inputs)),
        member_bindings_(std::move(member_bindings)),
        aggregate_binding_(std::move(aggregate_binding))
    {
        if (inputs_.empty() || inputs_.size() != member_bindings_.size())
            throw std::logic_error(
                "runtime sample output family requires one binding per member");
        layout_ = inputs_.front().channel_layout;
        for (auto const& input : inputs_)
            if (input.channel_layout != layout_)
                throw std::logic_error(
                    "runtime sample output family member layouts differ");
    }

    std::span<InputConfig const> inputs() const { return inputs_; }

    void declare(
        DeclarationContext<RuntimeSampleOutputFamilyNode> const& ctx) const
    {
        auto const sample_count =
            channel_count(layout_) * ctx.max_block_size();
        ctx.local_array(ctx.state().aggregate_samples, sample_count);
        ctx.local_array(
            ctx.state().member_samples,
            sample_count * inputs_.size());
    }

    void tick_block(
        TickBlockContext<RuntimeSampleOutputFamilyNode> const& ctx) const
    {
        auto const channels = channel_count(layout_);
        auto const sample_count = channels * ctx.block_size;
        auto aggregate = ctx.state().aggregate_samples.first(sample_count);
        std::fill(aggregate.begin(), aggregate.end(), Sample{0.0f});

        for (size_t member = 0; member < inputs_.size(); ++member) {
            auto const& binding = member_bindings_[member];
            auto const target_lane = binding
                ? binding->target_lane
                : LaneId{};
            auto const include_in_aggregate = binding &&
                binding->include_in_aggregate;
            if (!target_lane && !include_in_aggregate) continue;

            auto samples = ctx.state().member_samples.subspan(
                member * sample_count, sample_count);
            for (size_t channel = 0; channel < channels; ++channel)
                for (size_t frame = 0; frame < ctx.block_size; ++frame)
                    samples[channel * ctx.block_size + frame] =
                        ctx.inputs[member].get_frame(frame, channel);

            if (include_in_aggregate) {
                for (size_t i = 0; i < sample_count; ++i)
                    aggregate[i] += samples[i];
            }
            if (target_lane && binding->publish_sample_block)
                binding->publish_sample_block(
                    target_lane, samples,
                    ChannelLayout{
                        .channel_type = layout_.channel_type,
                        .sample_layout = SampleStreamLayout::planar,
                    },
                    ctx.block_size);
        }

        auto const aggregate_target = aggregate_binding_
            ? aggregate_binding_->target_lane
            : LaneId{};
        if (aggregate_target &&
            aggregate_binding_->publish_sample_block) {
            aggregate_binding_->publish_sample_block(
                aggregate_target, aggregate,
                ChannelLayout{
                    .channel_type = layout_.channel_type,
                    .sample_layout = SampleStreamLayout::planar,
                },
                ctx.block_size);
        }
    }

private:
    std::vector<InputConfig> inputs_ {};
    std::vector<std::shared_ptr<RuntimeOutputBinding>> member_bindings_ {};
    std::shared_ptr<RuntimeOutputBinding> aggregate_binding_ {};
    ChannelLayout layout_ {};
};

class RuntimeEventOutputFamilyNode {
public:
    struct State {
        std::span<TimedEvent> aggregate_events {};
        std::span<TimedEvent> member_events {};
        std::span<size_t> aggregate_cursors {};
    };

    RuntimeEventOutputFamilyNode(
        EventTypeId type,
        size_t member_count,
        std::vector<std::shared_ptr<RuntimeOutputBinding>> member_bindings,
        std::shared_ptr<RuntimeOutputBinding> aggregate_binding)
      : type_(type),
        event_inputs_(member_count, EventInputConfig{.type = type}),
        member_bindings_(std::move(member_bindings)),
        aggregate_binding_(std::move(aggregate_binding))
    {
        if (event_inputs_.empty() ||
            event_inputs_.size() != member_bindings_.size())
            throw std::logic_error(
                "runtime event output family requires one binding per member");
    }

    std::span<EventInputConfig const> event_inputs() const
    {
        return event_inputs_;
    }

    void declare(
        DeclarationContext<RuntimeEventOutputFamilyNode> const& ctx) const
    {
        auto const capacity = calculate_event_port_buffer_capacity(
            ctx.event_port_buffer_base_multiplier(), type_);
        ctx.local_array(ctx.state().aggregate_events, capacity);
        ctx.local_array(
            ctx.state().member_events,
            capacity * event_inputs_.size());
        ctx.local_array(
            ctx.state().aggregate_cursors,
            event_inputs_.size());
    }

    void tick_block(
        TickBlockContext<RuntimeEventOutputFamilyNode> const& ctx) const
    {
        for (size_t member = 0; member < event_inputs_.size(); ++member) {
            auto const& binding = member_bindings_[member];
            auto const target_lane = binding
                ? binding->target_lane
                : LaneId{};
            auto const include_in_aggregate = binding &&
                binding->include_in_aggregate;
            if (!target_lane && !include_in_aggregate) continue;

            auto const input =
                ctx.event_inputs[member].get_block(ctx.index, ctx.block_size);
            auto member_events = ctx.state().member_events.subspan(
                member * ctx.state().aggregate_events.size(),
                ctx.state().aggregate_events.size());
            auto const count = std::min(input.size(), member_events.size());
            for (size_t i = 0; i < count; ++i)
                member_events[i] = input[i];
            if (target_lane && binding->publish_event_block)
                binding->publish_event_block(
                    target_lane, member_events.first(count));

        }

        auto const aggregate_target = aggregate_binding_
            ? aggregate_binding_->target_lane
            : LaneId{};
        if (!aggregate_target ||
            !aggregate_binding_->publish_event_block) return;

        std::fill(
            ctx.state().aggregate_cursors.begin(),
            ctx.state().aggregate_cursors.end(), size_t{0});
        size_t aggregate_count = 0;
        while (aggregate_count < ctx.state().aggregate_events.size()) {
            size_t selected_member = event_inputs_.size();
            TimedEvent selected_event {};
            for (size_t member = 0; member < event_inputs_.size(); ++member) {
                auto const& binding = member_bindings_[member];
                if (!binding || !binding->include_in_aggregate) continue;
                auto const input = ctx.event_inputs[member].get_block(
                    ctx.index, ctx.block_size);
                auto const cursor = ctx.state().aggregate_cursors[member];
                if (cursor >= input.size()) continue;
                auto const event = input[cursor];
                if (selected_member == event_inputs_.size() ||
                    event.time < selected_event.time) {
                    selected_member = member;
                    selected_event = event;
                }
            }
            if (selected_member == event_inputs_.size()) break;
            ctx.state().aggregate_events[aggregate_count++] = selected_event;
            ++ctx.state().aggregate_cursors[selected_member];
        }
        aggregate_binding_->publish_event_block(
            aggregate_target,
            ctx.state().aggregate_events.first(aggregate_count));
    }

private:
    EventTypeId type_ = EventTypeId::empty;
    std::vector<EventInputConfig> event_inputs_ {};
    std::vector<std::shared_ptr<RuntimeOutputBinding>> member_bindings_ {};
    std::shared_ptr<RuntimeOutputBinding> aggregate_binding_ {};
};

} // namespace iv
