#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/runtime/graph_output_blocks_events.h>
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
                auto const block = ctx.realtime_sample_input(0).get_block();
                for (size_t i = 0; i < ctx.sample_count(); ++i) {
                    ctx.out().push(i < block.size() ? block[i] : Sample {});
                }
                return;
            }
            for (size_t i = 0; i < ctx.sample_count(); ++i) {
                ctx.out().push(value);
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
                auto const block = ctx.realtime_sample_input(0).get_block();
                for (size_t i = 0; i < ctx.sample_count(); ++i) {
                    ctx.out().push(i < block.size() ? block[i] : default_value);
                }
                return;
            }
            for (size_t i = 0; i < ctx.sample_count(); ++i) {
                ctx.out().push(default_value);
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

    // Timeline-facing output lane: pulls the block a DSP output sink published into
    // GraphOutputBlocks and emits it as this lane's output. Mirror of the input-lane
    // nodes, inverted: this lane's task depends_on the DSP task (carried via the lane
    // upsert's external_task_dependencies), so the block is always published first.
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
            GraphOutputBlocksSampleBlockBuilder builder;
            IV_INVOKE_SINGLETON_EVENT(
                iv_runtime_graph_output_blocks_sample_requested_event,
                lane,
                builder);
            auto const block = builder.build();
            for (size_t i = 0; i < ctx.sample_count(); ++i) {
                ctx.out().push(i < block.size() ? block[i] : Sample {});
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
            GraphOutputBlocksEventBlockBuilder builder;
            IV_INVOKE_SINGLETON_EVENT(
                iv_runtime_graph_output_blocks_event_requested_event,
                lane,
                builder);
            auto const events = builder.build();
            ctx.out().push_block(BlockView<TimedEvent const> {
                .first = events,
            });
        }
    };
}
