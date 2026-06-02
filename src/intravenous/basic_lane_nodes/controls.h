#pragma once

#include "lane_node/generate.h"
#include "runtime/lane_graph.h"

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

        void generate(TimelineGenerateContext<KnobLaneNode>& ctx)
        {
            if (!ctx.realtime_sample_inputs().empty() && ctx.realtime_sample_input(0).connected()) {
                auto const block = ctx.realtime_sample_input(0).get_block();
                for (size_t i = 0; i < ctx.count(); ++i) {
                    ctx.out().push(i < block.size() ? block[i] : Sample {});
                }
                return;
            }
            for (size_t i = 0; i < ctx.count(); ++i) {
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

        void generate(TimelineGenerateContext<GraphSampleInputLaneNode>& ctx)
        {
            if (!ctx.realtime_sample_inputs().empty() && ctx.realtime_sample_input(0).connected()) {
                auto const block = ctx.realtime_sample_input(0).get_block();
                for (size_t i = 0; i < ctx.count(); ++i) {
                    ctx.out().push(i < block.size() ? block[i] : default_value);
                }
                return;
            }
            for (size_t i = 0; i < ctx.count(); ++i) {
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

        void generate(TimelineGenerateContext<GraphEventInputLaneNode>& ctx)
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
}
