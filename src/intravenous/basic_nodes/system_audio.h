#pragma once

#include <intravenous/node/lifecycle.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>

namespace iv {

inline constexpr ChannelLayout system_audio_channel_layout{
    .channel_type = ChannelTypeId::stereo,
    .sample_layout = SampleStreamLayout::interleaved,
};

struct SystemAudioInput {
    char const* device_id = "default";

    struct State {
        SystemAudioInputBinding binding{};

        // The binding only names the SystemAudioDevices binding for
        // device_id, and resolving one is not safe on the audio thread, where
        // a move would run. State is therefore not movable: a successor runs
        // initialize() before the edit activates and resolves its own
        // device_id.
        State() = default;
        State(State&&) = delete;
    };

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{output<Sample>(
            "out",
            OutputProduction::tick,
            {.channel_layout = system_audio_channel_layout})};
    }

    void initialize(InitializationContext<SystemAudioInput> const& ctx) const
    {
        ctx.state().binding =
            ctx.resources.system_audio.resolve_input(
                device_id ? std::string_view{device_id} : std::string_view{});
    }

    void tick_block(TickBlockContext<SystemAudioInput> const& ctx) const
    {
        auto const source = ctx.state().binding.captured_samples();
        auto output = ctx.output<"out">();
        auto constexpr channels = channel_count(
            system_audio_channel_layout.channel_type);
        for (std::size_t frame = 0; frame < output.block_size(); ++frame) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto const sample = frame * channels + channel;
                output.value(frame, channel) = sample < source.size()
                    ? source[sample]
                    : Sample{};
            }
        }
    }
};

struct SystemAudioOutput {
    char const* device_id = "default";

    struct State {
        SystemAudioOutputBinding binding{};

        // The binding only names the SystemAudioDevices binding for
        // device_id, and resolving one is not safe on the audio thread, where
        // a move would run. State is therefore not movable: a successor runs
        // initialize() before the edit activates and resolves its own
        // device_id.
        State() = default;
        State(State&&) = delete;
    };

    static constexpr auto inputs()
    {
        return std::array{input<Sample>(
            "in",
            InputAccess::sequential,
            {.channel_layout = system_audio_channel_layout})};
    }
    static constexpr auto outputs() { return std::tuple{}; }

    void initialize(InitializationContext<SystemAudioOutput> const& ctx) const
    {
        ctx.state().binding =
            ctx.resources.system_audio.resolve_output(
                device_id ? std::string_view{device_id} : std::string_view{});
    }

    void tick_block(TickBlockContext<SystemAudioOutput> const& ctx) const
    {
        auto destination = ctx.state().binding.render_samples();
        auto input = ctx.input<"in">();
        auto constexpr channels = channel_count(
            system_audio_channel_layout.channel_type);
        auto const frames = std::min(
            input.block_size(), destination.size() / channels);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                destination[frame * channels + channel].value +=
                    input.get_value(frame, channel).value;
            }
        }
    }
};

} // namespace iv
