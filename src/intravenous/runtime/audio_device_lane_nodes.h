#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/runtime/audio_device_lanes_events.h>

namespace iv {

struct AudioDeviceOutputLaneNode {
    static std::array<RealtimeSampleLaneInputConfig, 1> realtime_sample_inputs()
    {
        return {
            RealtimeSampleLaneInputConfig{
                .name = "source",
                .sample_layout = SampleStreamLayout::interleaved,
            },
        };
    }

    static RealtimeSampleLaneOutputConfig output()
    {
        return RealtimeSampleLaneOutputConfig{
            .name = "output",
            .sample_layout = SampleStreamLayout::interleaved,
        };
    }

    void tick_block_realtime(RealtimeLaneTickContext<AudioDeviceOutputLaneNode> &ctx)
    {
        auto const out = ctx.out().block_view();
        if (ctx.realtime_sample_input(0).connected()) {
            ctx.out().write_block(ctx.realtime_sample_input(0).block_view());
            return;
        }
        for (size_t frame = 0; frame < out.frames(); ++frame) {
            for (size_t channel = 0; channel < out.channels(); ++channel) {
                out.set(frame, channel, Sample {});
            }
        }
    }
};

struct AudioDeviceInputLaneNode {
    LaneId lane {};

    static RealtimeSampleLaneOutputConfig output()
    {
        return RealtimeSampleLaneOutputConfig{
            .name = "output",
            .sample_layout = SampleStreamLayout::interleaved,
        };
    }

    void tick_block_realtime(RealtimeLaneTickContext<AudioDeviceInputLaneNode> &ctx)
    {
        AudioDeviceLanesInputBlockBuilder builder;
        IV_INVOKE_SINGLETON_EVENT(
            iv_runtime_audio_device_lanes_input_block_requested_event,
            lane,
            builder);
        auto const block = builder.build();
        auto const out = ctx.out().block_view();
        if (!block.empty()) {
            ctx.out().write_block(block.view());
            return;
        }
        for (size_t frame = 0; frame < out.frames(); ++frame) {
            for (size_t channel = 0; channel < out.channels(); ++channel) {
                out.set(frame, channel, Sample {});
            }
        }
    }
};

} // namespace iv
