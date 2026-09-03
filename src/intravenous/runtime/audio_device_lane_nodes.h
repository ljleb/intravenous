#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/runtime/audio_device_lanes_events.h>

namespace iv {

struct AudioDeviceOutputLaneNode {
    static auto ports()
    {
        return std::array{
            LanePortConfig{
                .name = "source",
                .domain = RealtimeLanePortConfig{},
                .direction = LaneInputPortConfig{
                    .type = SampleLaneInputPortConfig{
                        .sample_layout = SampleStreamLayout::interleaved,
                    },
                },
            },
            LanePortConfig{
                .name = "output",
                .domain = RealtimeLanePortConfig{},
                .direction = LaneOutputPortConfig{
                    .type = SampleLaneOutputPortConfig{
                        .sample_layout = SampleStreamLayout::interleaved,
                    },
                },
            },
        };
    }

    void tick_block_realtime(RealtimeLaneTickContext<AudioDeviceOutputLaneNode> &ctx)
    {
        auto &output = std::get<RealtimeSampleLaneOutput>(ctx.out());
        auto const out = output.block_view();
        if (ctx.realtime_sample_input(0).connected()) {
            output.write_block(ctx.realtime_sample_input(0).block_view());
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

    static auto ports()
    {
        return std::array{
            LanePortConfig{
                .name = "output",
                .domain = RealtimeLanePortConfig{},
                .direction = LaneOutputPortConfig{
                    .type = SampleLaneOutputPortConfig{
                        .sample_layout = SampleStreamLayout::interleaved,
                    },
                },
            },
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
        auto &output = std::get<RealtimeSampleLaneOutput>(ctx.out());
        auto const out = output.block_view();
        if (!block.empty()) {
            output.write_block(block.view());
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
