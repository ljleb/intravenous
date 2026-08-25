#pragma once

#include <intravenous/graph/runtime_bindings.h>
#include <intravenous/graph/static_storage.hpp>
#include <intravenous/node/lifecycle.h>

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace iv {
struct RuntimeSampleInputNodeSpec {
    OutputConfig output {};
    Sample default_value = 0.0f;
    std::string binding_id {};

    constexpr auto outputs() const
    {
        return std::array<OutputConfig, 1>{ output };
    }
};

struct RuntimeEventInputNodeSpec {
    EventTypeId type = EventTypeId::empty;
    std::string binding_id {};

    constexpr auto event_outputs() const
    {
        return std::array<EventOutputConfig, 1>{{ { .type = type } }};
    }
};

struct RuntimeSampleOutputNodeSpec {
    InputConfig input {};
    std::string binding_id {};

    constexpr auto inputs() const
    {
        return std::array<InputConfig, 1>{ input };
    }
};

struct RuntimeEventOutputNodeSpec {
    EventTypeId type = EventTypeId::empty;
    std::string binding_id {};

    constexpr auto event_inputs() const
    {
        return std::array<EventInputConfig, 1>{{ { .type = type } }};
    }
};

struct RuntimeSampleOutputFamilyNodeSpec {
    std::vector<InputConfig> input_configs {};
    std::vector<std::string> member_binding_ids {};
    std::string aggregate_binding_id {};

    constexpr std::vector<InputConfig> const& inputs() const
    {
        return input_configs;
    }
};

struct RuntimeEventOutputFamilyNodeSpec {
    EventTypeId type = EventTypeId::empty;
    size_t member_count = 0;
    std::vector<std::string> member_binding_ids {};
    std::string aggregate_binding_id {};

    constexpr std::vector<EventInputConfig> event_inputs() const
    {
        return std::vector<EventInputConfig>(
            member_count,
            EventInputConfig{ .type = type });
    }
};

struct StaticRuntimeInputConfig {
    StaticString name {};
    ChannelLayout channel_layout {
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };
    size_t history = 0;
    Sample default_value = 0.0;
    Sample min = -std::numeric_limits<Sample::storage>::infinity();
    Sample max = std::numeric_limits<Sample::storage>::infinity();

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

struct StaticRuntimeOutputConfig {
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

struct RuntimeSampleInputNode {
    StaticRuntimeOutputConfig output {};
    Sample default_value = 0.0f;
    StaticString binding_id {};

    struct State {
        std::span<Sample> samples {};
        std::span<RuntimeSampleInputBinding> binding {};
    };

    constexpr auto outputs() const
    {
        return std::array<OutputConfig, 1>{ output.config() };
    }

    void declare(DeclarationContext<RuntimeSampleInputNode> const& ctx) const
    {
        ctx.local_array(
            ctx.state().samples,
            channel_count(output.channel_layout) * ctx.max_block_size());
        ctx.local_array(ctx.state().binding, 1);
        ctx.export_array(std::string(binding_id.view()), ctx.state().binding);
    }

    void tick_block(TickBlockContext<RuntimeSampleInputNode> const& ctx) const
    {
        auto const channels = channel_count(output.channel_layout);
        auto samples = ctx.state().samples.first(channels * ctx.block_size);
        auto const& binding = ctx.state().binding.front();
        if (binding.mode == RuntimeSampleInputMode::timeline) {
            std::fill(samples.begin(), samples.end(), Sample{0.0f});
            if (binding.read_timeline_block) {
                binding.read_timeline_block(
                    binding.timeline_lane,
                    binding.source_channel,
                    0,
                    ctx.index,
                    ctx.block_size,
                    output.channel_layout,
                    samples);
            }
        } else {
            auto const value = binding.mode == RuntimeSampleInputMode::scalar
                ? binding.value
                : default_value;
            std::fill(samples.begin(), samples.end(), value);
        }

        for (size_t channel = 0; channel < channels; ++channel)
            for (size_t frame = 0; frame < ctx.block_size; ++frame)
                ctx.outputs[0].write_frame(
                    frame, channel, samples[channel * ctx.block_size + frame]);
    }
};

struct RuntimeEventInputNode {
    EventTypeId type = EventTypeId::empty;
    StaticString binding_id {};

    struct State {
        std::span<RuntimeEventInputBinding> binding {};
    };

    constexpr auto event_outputs() const
    {
        return std::array<EventOutputConfig, 1>{{ { .type = type } }};
    }

    void declare(DeclarationContext<RuntimeEventInputNode> const& ctx) const
    {
        ctx.local_array(ctx.state().binding, 1);
        ctx.export_array(std::string(binding_id.view()), ctx.state().binding);
    }

    void tick_block(TickBlockContext<RuntimeEventInputNode> const& ctx) const
    {
        auto const& binding = ctx.state().binding.front();
        if (!binding.read_timeline_block || !binding.timeline_lane) return;
        auto const view = binding.read_timeline_block(
            binding.timeline_lane, ctx.index, ctx.block_size);
        for (size_t i = 0; i < view.size; ++i)
            ctx.event_outputs[0].push(view.data[i]);
    }
};

struct RuntimeSampleOutputNode {
    StaticRuntimeInputConfig input {};
    StaticString binding_id {};

    struct State {
        std::span<Sample> samples {};
        std::span<RuntimeOutputBinding> binding {};
    };

    constexpr auto inputs() const
    {
        return std::array<InputConfig, 1>{ input.config() };
    }

    void declare(DeclarationContext<RuntimeSampleOutputNode> const& ctx) const
    {
        ctx.local_array(
            ctx.state().samples,
            channel_count(input.channel_layout) * ctx.max_block_size());
        ctx.local_array(ctx.state().binding, 1);
        ctx.export_array(std::string(binding_id.view()), ctx.state().binding);
    }

    void tick_block(TickBlockContext<RuntimeSampleOutputNode> const& ctx) const
    {
        auto const& binding = ctx.state().binding.front();
        if (!binding.publish_sample_block || !binding.target_lane) return;
        auto const channels = channel_count(input.channel_layout);
        auto samples = ctx.state().samples.first(channels * ctx.block_size);
        for (size_t channel = 0; channel < channels; ++channel)
            for (size_t frame = 0; frame < ctx.block_size; ++frame)
                samples[channel * ctx.block_size + frame] =
                    ctx.inputs[0].get_frame(frame, channel);
        binding.publish_sample_block(
            binding.target_lane,
            samples,
            ChannelLayout{
                .channel_type = input.channel_layout.channel_type,
                .sample_layout = SampleStreamLayout::planar,
            },
            ctx.block_size);
    }
};

struct RuntimeEventOutputNode {
    EventTypeId type = EventTypeId::empty;
    StaticString binding_id {};

    struct State {
        std::span<TimedEvent> events {};
        std::span<RuntimeOutputBinding> binding {};
    };

    constexpr auto event_inputs() const
    {
        return std::array<EventInputConfig, 1>{{ { .type = type } }};
    }

    void declare(DeclarationContext<RuntimeEventOutputNode> const& ctx) const
    {
        ctx.local_array(
            ctx.state().events,
            calculate_event_port_buffer_capacity(
                ctx.event_port_buffer_base_multiplier(), type));
        ctx.local_array(ctx.state().binding, 1);
        ctx.export_array(std::string(binding_id.view()), ctx.state().binding);
    }

    void tick_block(TickBlockContext<RuntimeEventOutputNode> const& ctx) const
    {
        auto const& binding = ctx.state().binding.front();
        if (!binding.publish_event_block || !binding.target_lane) return;
        auto const input =
            ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
        auto const count = std::min(input.size(), ctx.state().events.size());
        for (size_t i = 0; i < count; ++i)
            ctx.state().events[i] = input[i];
        binding.publish_event_block(
            binding.target_lane,
            ctx.state().events.first(count));
    }
};

struct RuntimeSampleOutputFamilyNode {
    StaticSpan<StaticRuntimeInputConfig> input_configs {};
    StaticSpan<StaticString> member_binding_ids {};
    StaticString aggregate_binding_id {};
    ChannelLayout layout {};

    struct State {
        std::span<Sample> aggregate_samples {};
        std::span<Sample> member_samples {};
        std::span<RuntimeOutputBinding> member_bindings {};
        std::span<RuntimeOutputBinding> aggregate_binding {};
    };

    constexpr std::vector<InputConfig> inputs() const
    {
        std::vector<InputConfig> result;
        result.reserve(input_configs.size);
        for (auto const& input : input_configs)
            result.push_back(input.config());
        return result;
    }

    void declare(
        DeclarationContext<RuntimeSampleOutputFamilyNode> const& ctx) const
    {
        auto const sample_count =
            channel_count(layout) * ctx.max_block_size();
        ctx.local_array(ctx.state().aggregate_samples, sample_count);
        ctx.local_array(
            ctx.state().member_samples,
            sample_count * input_configs.size);
        ctx.local_array(
            ctx.state().member_bindings,
            member_binding_ids.size);
        for (auto const id : member_binding_ids)
            ctx.export_array(
                std::string(id.view()), ctx.state().member_bindings);
        ctx.local_array(ctx.state().aggregate_binding, 1);
        ctx.export_array(
            std::string(aggregate_binding_id.view()),
            ctx.state().aggregate_binding);
    }

    void tick_block(
        TickBlockContext<RuntimeSampleOutputFamilyNode> const& ctx) const
    {
        auto const channels = channel_count(layout);
        auto const sample_count = channels * ctx.block_size;
        auto aggregate = ctx.state().aggregate_samples.first(sample_count);
        std::fill(aggregate.begin(), aggregate.end(), Sample{0.0f});

        for (size_t member = 0; member < input_configs.size; ++member) {
            auto const& binding = ctx.state().member_bindings[member];
            if (!binding.target_lane && !binding.include_in_aggregate) continue;

            auto samples = ctx.state().member_samples.subspan(
                member * sample_count, sample_count);
            for (size_t channel = 0; channel < channels; ++channel)
                for (size_t frame = 0; frame < ctx.block_size; ++frame)
                    samples[channel * ctx.block_size + frame] =
                        ctx.inputs[member].get_frame(frame, channel);

            if (binding.include_in_aggregate)
                for (size_t i = 0; i < sample_count; ++i)
                    aggregate[i] += samples[i];
            if (binding.target_lane && binding.publish_sample_block)
                binding.publish_sample_block(
                    binding.target_lane,
                    samples,
                    ChannelLayout{
                        .channel_type = layout.channel_type,
                        .sample_layout = SampleStreamLayout::planar,
                    },
                    ctx.block_size);
        }

        auto const& aggregate_binding = ctx.state().aggregate_binding.front();
        if (aggregate_binding.target_lane
            && aggregate_binding.publish_sample_block) {
            aggregate_binding.publish_sample_block(
                aggregate_binding.target_lane,
                aggregate,
                ChannelLayout{
                    .channel_type = layout.channel_type,
                    .sample_layout = SampleStreamLayout::planar,
                },
                ctx.block_size);
        }
    }
};

struct RuntimeEventOutputFamilyNode {
    EventTypeId type = EventTypeId::empty;
    size_t member_count = 0;
    StaticSpan<StaticString> member_binding_ids {};
    StaticString aggregate_binding_id {};

    struct State {
        std::span<TimedEvent> aggregate_events {};
        std::span<TimedEvent> member_events {};
        std::span<size_t> aggregate_cursors {};
        std::span<RuntimeOutputBinding> member_bindings {};
        std::span<RuntimeOutputBinding> aggregate_binding {};
    };

    constexpr std::vector<EventInputConfig> event_inputs() const
    {
        return std::vector<EventInputConfig>(
            member_count,
            EventInputConfig{ .type = type });
    }

    void declare(
        DeclarationContext<RuntimeEventOutputFamilyNode> const& ctx) const
    {
        auto const capacity = calculate_event_port_buffer_capacity(
            ctx.event_port_buffer_base_multiplier(), type);
        ctx.local_array(ctx.state().aggregate_events, capacity);
        ctx.local_array(ctx.state().member_events, capacity * member_count);
        ctx.local_array(ctx.state().aggregate_cursors, member_count);
        ctx.local_array(ctx.state().member_bindings, member_count);
        for (auto const id : member_binding_ids)
            ctx.export_array(
                std::string(id.view()), ctx.state().member_bindings);
        ctx.local_array(ctx.state().aggregate_binding, 1);
        ctx.export_array(
            std::string(aggregate_binding_id.view()),
            ctx.state().aggregate_binding);
    }

    void tick_block(
        TickBlockContext<RuntimeEventOutputFamilyNode> const& ctx) const
    {
        for (size_t member = 0; member < member_count; ++member) {
            auto const& binding = ctx.state().member_bindings[member];
            if (!binding.target_lane && !binding.include_in_aggregate) continue;

            auto const input =
                ctx.event_inputs[member].get_block(ctx.index, ctx.block_size);
            auto member_events = ctx.state().member_events.subspan(
                member * ctx.state().aggregate_events.size(),
                ctx.state().aggregate_events.size());
            auto const count = std::min(input.size(), member_events.size());
            for (size_t i = 0; i < count; ++i)
                member_events[i] = input[i];
            if (binding.target_lane && binding.publish_event_block)
                binding.publish_event_block(
                    binding.target_lane,
                    member_events.first(count));
        }

        auto const& aggregate_binding = ctx.state().aggregate_binding.front();
        if (!aggregate_binding.target_lane
            || !aggregate_binding.publish_event_block) return;

        std::fill(
            ctx.state().aggregate_cursors.begin(),
            ctx.state().aggregate_cursors.end(), size_t{0});
        size_t aggregate_count = 0;
        while (aggregate_count < ctx.state().aggregate_events.size()) {
            size_t selected_member = member_count;
            TimedEvent selected_event {};
            for (size_t member = 0; member < member_count; ++member) {
                auto const& binding = ctx.state().member_bindings[member];
                if (!binding.include_in_aggregate) continue;
                auto const input = ctx.event_inputs[member].get_block(
                    ctx.index, ctx.block_size);
                auto const cursor = ctx.state().aggregate_cursors[member];
                if (cursor >= input.size()) continue;
                auto const event = input[cursor];
                if (selected_member == member_count
                    || event.time < selected_event.time) {
                    selected_member = member;
                    selected_event = event;
                }
            }
            if (selected_member == member_count) break;
            ctx.state().aggregate_events[aggregate_count++] = selected_event;
            ++ctx.state().aggregate_cursors[selected_member];
        }
        aggregate_binding.publish_event_block(
            aggregate_binding.target_lane,
            ctx.state().aggregate_events.first(aggregate_count));
    }
};

namespace details {
consteval StaticRuntimeInputConfig freeze_runtime_input_config(
    InputConfig const& config)
{
    return {
        .name = define_static_string(config.name),
        .channel_layout = config.channel_layout,
        .history = config.history,
        .default_value = config.default_value,
        .min = config.min,
        .max = config.max,
    };
}

consteval StaticRuntimeOutputConfig freeze_runtime_output_config(
    OutputConfig const& config)
{
    return {
        .name = define_static_string(config.name),
        .channel_layout = config.channel_layout,
        .latency = config.latency,
        .history = config.history,
    };
}

consteval RuntimeSampleInputNode freeze_generated_node(
    RuntimeSampleInputNodeSpec const& spec)
{
    return {
        .output = freeze_runtime_output_config(spec.output),
        .default_value = spec.default_value,
        .binding_id = define_static_string(spec.binding_id),
    };
}

consteval RuntimeEventInputNode freeze_generated_node(
    RuntimeEventInputNodeSpec const& spec)
{
    return {
        .type = spec.type,
        .binding_id = define_static_string(spec.binding_id),
    };
}

consteval RuntimeSampleOutputNode freeze_generated_node(
    RuntimeSampleOutputNodeSpec const& spec)
{
    return {
        .input = freeze_runtime_input_config(spec.input),
        .binding_id = define_static_string(spec.binding_id),
    };
}

consteval RuntimeEventOutputNode freeze_generated_node(
    RuntimeEventOutputNodeSpec const& spec)
{
    return {
        .type = spec.type,
        .binding_id = define_static_string(spec.binding_id),
    };
}

consteval RuntimeSampleOutputFamilyNode freeze_generated_node(
    RuntimeSampleOutputFamilyNodeSpec const& spec)
{
    if (spec.input_configs.empty()
        || spec.input_configs.size() != spec.member_binding_ids.size()) {
        throw std::logic_error(
            "runtime sample output family requires one binding per member");
    }
    auto const layout = spec.input_configs.front().channel_layout;
    std::vector<StaticRuntimeInputConfig> inputs;
    inputs.reserve(spec.input_configs.size());
    for (auto const& input : spec.input_configs) {
        if (input.channel_layout != layout) {
            throw std::logic_error(
                "runtime sample output family member layouts differ");
        }
        inputs.push_back(freeze_runtime_input_config(input));
    }
    std::vector<StaticString> member_binding_ids;
    member_binding_ids.reserve(spec.member_binding_ids.size());
    for (auto const& id : spec.member_binding_ids)
        member_binding_ids.push_back(define_static_string(id));
    return {
        .input_configs = define_static_span(inputs),
        .member_binding_ids = define_static_span(member_binding_ids),
        .aggregate_binding_id =
            define_static_string(spec.aggregate_binding_id),
        .layout = layout,
    };
}

consteval RuntimeEventOutputFamilyNode freeze_generated_node(
    RuntimeEventOutputFamilyNodeSpec const& spec)
{
    if (spec.member_count == 0
        || spec.member_count != spec.member_binding_ids.size()) {
        throw std::logic_error(
            "runtime event output family requires one binding per member");
    }
    std::vector<StaticString> member_binding_ids;
    member_binding_ids.reserve(spec.member_binding_ids.size());
    for (auto const& id : spec.member_binding_ids)
        member_binding_ids.push_back(define_static_string(id));
    return {
        .type = spec.type,
        .member_count = spec.member_count,
        .member_binding_ids = define_static_span(member_binding_ids),
        .aggregate_binding_id =
            define_static_string(spec.aggregate_binding_id),
    };
}
} // namespace details
} // namespace iv
