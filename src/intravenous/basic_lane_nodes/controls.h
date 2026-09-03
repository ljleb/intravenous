#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/lane_graph.h>

#include <array>
#include <string>

namespace iv {
    struct KnobLaneNode {
        Sample value = 0.0f;
        std::string name {"value"};

        std::array<LanePortConfig, 2> ports() const
        {
            return {
                sample_input_port("value", LanePortDomain::realtime),
                sample_output_port(name, LanePortDomain::realtime),
            };
        }

        void tick_block_realtime(RealtimeLaneTickContext<KnobLaneNode>& ctx)
        {
            auto& output = std::get<RealtimeSampleLaneOutput>(ctx.out());
            if (!ctx.realtime_sample_inputs().empty() && ctx.realtime_sample_input(0).connected()) {
                output.write_block(ctx.realtime_sample_input(0).block_view());
                return;
            }
            auto const out = output.block_view();
            for (size_t frame = 0; frame < out.frames(); ++frame) {
                for (size_t channel = 0; channel < out.channels(); ++channel) {
                    out.set(frame, channel, value);
                }
            }
        }
    };

    struct GraphSampleInputLaneNode {
        Sample default_value = 0.0f;
        std::string name {"value"};

        std::array<LanePortConfig, 2> ports() const
        {
            return {
                sample_input_port("value", LanePortDomain::realtime, default_value),
                sample_output_port(name, LanePortDomain::realtime),
            };
        }

        void tick_block_realtime(RealtimeLaneTickContext<GraphSampleInputLaneNode>& ctx)
        {
            auto& output = std::get<RealtimeSampleLaneOutput>(ctx.out());
            if (!ctx.realtime_sample_inputs().empty() && ctx.realtime_sample_input(0).connected()) {
                auto const input = ctx.realtime_sample_input(0).block_view();
                auto const out = output.block_view();
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
            auto const out = output.block_view();
            for (size_t frame = 0; frame < out.frames(); ++frame) {
                for (size_t channel = 0; channel < out.channels(); ++channel) {
                    out.set(frame, channel, default_value);
                }
            }
        }
    };

    struct GraphEventInputLaneNode {
        std::array<LanePortConfig, 2> ports() const
        {
            return {
                event_input_port("events", LanePortDomain::realtime),
                event_output_port("events", LanePortDomain::realtime),
            };
        }

        void tick_block_realtime(RealtimeLaneTickContext<GraphEventInputLaneNode>& ctx)
        {
            if (ctx.realtime_event_inputs().empty()) {
                return;
            }
            auto const events = ctx.realtime_event_input(0).get_block();
            std::get<RealtimeEventLaneOutput>(ctx.out()).push_block(BlockView<TimedEvent const> {
                .first = events,
            });
        }
    };

    struct GraphSampleOutputLaneNode {
        LaneId lane {};
        std::string name {"value"};

        std::array<LanePortConfig, 1> ports() const
        {
            return { sample_output_port(name, LanePortDomain::realtime) };
        }

        void tick_block_realtime(RealtimeLaneTickContext<GraphSampleOutputLaneNode>& ctx)
        {
            auto& output = std::get<RealtimeSampleLaneOutput>(ctx.out());
            GraphInputLanesSampleBlockBuilder builder;
            IV_INVOKE_SINGLETON_EVENT(
                iv_runtime_graph_input_lanes_sample_block_requested_event,
                lane,
                builder);
            auto const block = builder.build();
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

    struct GraphEventOutputLaneNode {
        LaneId lane {};

        std::array<LanePortConfig, 1> ports() const
        {
            return { event_output_port("events", LanePortDomain::realtime) };
        }

        void tick_block_realtime(RealtimeLaneTickContext<GraphEventOutputLaneNode>& ctx)
        {
            GraphInputLanesEventBlockBuilder builder;
            IV_INVOKE_SINGLETON_EVENT(
                iv_runtime_graph_input_lanes_event_block_requested_event,
                lane,
                builder);
            auto const events = builder.build();
            std::get<RealtimeEventLaneOutput>(ctx.out()).push_block(BlockView<TimedEvent const> {
                .first = events,
            });
        }
    };
}
