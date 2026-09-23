#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/runtime/lane_graph.h>

#include <array>
#include <string>

namespace iv {
    struct KnobLaneNode {
        Sample value = 0.0f;
        std::string name {"value"};

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
                .name = name,
            };
        }

        void tick_block_realtime(RealtimeLaneTickContext<KnobLaneNode>& ctx)
        {
            if (!ctx.realtime_sample_inputs().empty() && ctx.sequential_sample_input(0).connected()) {
                ctx.out().write_block(ctx.sequential_sample_input(0).block_view());
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

}
