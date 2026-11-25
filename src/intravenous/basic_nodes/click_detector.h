#pragma once

#include <intravenous/basic_nodes/system_audio.h>

#include <array>
#include <cmath>
#include <optional>

namespace iv {

struct ClickDetector {
    static constexpr std::size_t minimum_event_spacing = 32;
    static constexpr float default_threshold = 1.0f / 256.0f;

    struct State {
        struct Channel {
            float sample = 0;
            float first = 0;
            float second = 0;
            float third = 0;

            float advance(float value) noexcept
            {
                auto const next_first = value - sample;
                auto const next_second = next_first - first;
                auto const next_third = next_second - second;
                auto const difference = next_third - third;
                sample = value;
                first = next_first;
                second = next_second;
                third = next_third;
                return std::abs(difference);
            }
        };
        std::array<Channel, 2> channels{};
        std::optional<GlobalIndex> next_block{};
        std::size_t warmup = 4;
        std::size_t cooldown = 0;

        bool detect(float left, float right, float threshold) noexcept
        {
            if (!std::isfinite(left) || !std::isfinite(right)) {
                channels = {};
                warmup = 4;
                return false;
            }
            // The fourth difference suppresses smooth, low-frequency content.
            auto const left_difference = channels[0].advance(left);
            auto const right_difference = channels[1].advance(right);
            if (warmup) {
                --warmup;
                return false;
            }
            if (threshold > 0 && (left_difference >= threshold || right_difference >= threshold)) {
                auto const starts_burst = cooldown == 0;
                cooldown = minimum_event_spacing - 1;
                return starts_burst;
            }
            if (cooldown) --cooldown;
            return false;
        }
    };

    static constexpr auto inputs()
    {
        return std::array{
            input<Sample>("audio", InputAccess::sequential, {.channel_layout = system_audio_channel_layout}),
            input<Sample>("threshold", {.default_value = default_threshold, .min = 0.0f}),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{event_output("clicks", EventTypeId::trigger,
            OutputProduction::sequential, OutputRealization::buffered,
            {.max_events_per_index = 1.0 / minimum_event_spacing})};
    }

    void tick_block(TickBlockContext<ClickDetector> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = ctx.index.whole();
        if (state.next_block != index) state = {};
        state.next_block = index.advanced_by(ctx.block_size);
        auto audio = ctx.input<"audio">();
        auto threshold = ctx.input<"threshold">();
        auto clicks = ctx.output<"clicks">();
        // Only blocks that contain detections enter the recording.
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            if (state.detect(audio.get_value(i, 0).value, audio.get_value(i, 1).value,
                    threshold.get_value(i).value)) {
                clicks.push_and_append_to_recording_gaps(TriggerEvent{}, i);
            }
        }
    }
};

} // namespace iv
