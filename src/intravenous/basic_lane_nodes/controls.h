#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/lane_graph.h>

#include <array>

namespace iv {
    struct KnobLaneNode {
        Sample value = 0.0f;

        std::array<RealtimeSampleLaneInputConfig, 1> realtime_sample_inputs() const
        {
            return {
                RealtimeSampleLaneInputConfig {
                    .name = "value",
                },
            };
        }

        RealtimeSampleLaneOutputConfig output() const
        {
            return RealtimeSampleLaneOutputConfig {
                .name = "value",
            };
        }

        void tick_block_realtime(RealtimeLaneTickContext<KnobLaneNode>& ctx)
        {
            if (!ctx.realtime_sample_inputs().empty() && ctx.realtime_sample_input(0).connected()) {
                ctx.out().write_block(ctx.realtime_sample_input(0).block_view());
                return;
            }
            auto const out = ctx.out().block_view();
            for (size_t frame = 0; frame < out.frames(); ++frame) {
                for (size_t channel = 0; channel < out.channels(); ++channel) {
                    out.set(frame, channel, value);
                }
            }
        }
    };

    struct GraphSampleInputLaneNode {
        Sample default_value = 0.0f;

        std::array<RealtimeSampleLaneInputConfig, 1> realtime_sample_inputs() const
        {
            return {
                RealtimeSampleLaneInputConfig {
                    .name = "value",
                    .default_value = default_value,
                },
            };
        }

        RealtimeSampleLaneOutputConfig output() const
        {
            return RealtimeSampleLaneOutputConfig {
                .name = "value",
            };
        }

        void tick_block_realtime(RealtimeLaneTickContext<GraphSampleInputLaneNode>& ctx)
        {
            if (!ctx.realtime_sample_inputs().empty() && ctx.realtime_sample_input(0).connected()) {
                auto const input = ctx.realtime_sample_input(0).block_view();
                auto const out = ctx.out().block_view();
                for (size_t frame = 0; frame < out.frames(); ++frame) {
                    for (size_t channel = 0; channel < out.channels(); ++channel) {
                        if (channel < input.channels()) {
                            out.set(frame, channel, input.get(frame, channel));
                        } else {
                            out.set(frame, channel, default_value);
                        }
                    }
                }
                return;
            }
            auto const out = ctx.out().block_view();
            for (size_t frame = 0; frame < out.frames(); ++frame) {
                for (size_t channel = 0; channel < out.channels(); ++channel) {
                    out.set(frame, channel, default_value);
                }
            }
        }
    };

    struct GraphEventInputLaneNode {
        std::array<RealtimeEventLaneInputConfig, 1> realtime_event_inputs() const
        {
            return {
                RealtimeEventLaneInputConfig {
                    .name = "events",
                },
            };
        }

        RealtimeEventLaneOutputConfig output() const
        {
            return RealtimeEventLaneOutputConfig {
                .name = "events",
            };
        }

        void tick_block_realtime(RealtimeLaneTickContext<GraphEventInputLaneNode>& ctx)
        {
            if (ctx.realtime_event_inputs().empty()) {
                return;
            }
            auto const events = ctx.realtime_event_input(0).get_block();
            ctx.out().push_block(BlockView<TimedEvent const> {
                .first = events,
            });
        }
    };

    struct GraphSampleOutputLaneNode {
        LaneId lane {};

        RealtimeSampleLaneOutputConfig output() const
        {
            return RealtimeSampleLaneOutputConfig {
                .name = "value",
            };
        }

        void tick_block_realtime(RealtimeLaneTickContext<GraphSampleOutputLaneNode>& ctx)
        {
            GraphInputLanesSampleBlockBuilder builder;
            IV_INVOKE_SINGLETON_EVENT(
                iv_runtime_graph_input_lanes_sample_block_requested_event,
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

    struct GraphEventOutputLaneNode {
        LaneId lane {};

        RealtimeEventLaneOutputConfig output() const
        {
            return RealtimeEventLaneOutputConfig {
                .name = "events",
            };
        }

        void tick_block_realtime(RealtimeLaneTickContext<GraphEventOutputLaneNode>& ctx)
        {
            GraphInputLanesEventBlockBuilder builder;
            IV_INVOKE_SINGLETON_EVENT(
                iv_runtime_graph_input_lanes_event_block_requested_event,
                lane,
                builder);
            auto const events = builder.build();
            ctx.out().push_block(BlockView<TimedEvent const> {
                .first = events,
            });
        }
    };
}
