#pragma once

#include "node_lifecycle.h"

#include <array>
#include <string>

namespace iv {
    struct AudioDeviceSink {
        size_t device_id = 0;
        size_t channel = 0;

        struct State {
            std::span<Sample> buffer;
        };

        auto inputs() const
        {
            return std::array{
                InputConfig{ "in" }
            };
        }

        void declare(DeclarationContext<AudioDeviceSink> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.buffer, ctx.max_block_size());
        }

        void initialize(InitializationContext<AudioDeviceSink> const& ctx) const
        {
            if (!ctx.execution_targets) {
                return;
            }

            auto& state = ctx.state();
            auto& target = ctx.execution_targets->audio_device(device_id, channel);
            target.register_sink(state.buffer);
        }

        void move(MoveContext<AudioDeviceSink> const& ctx) const
        {
            if (!ctx.execution_targets) {
                return;
            }

            auto& state = ctx.state();
            auto& previous = ctx.previous_state();
            auto& target = ctx.execution_targets->audio_device(device_id, channel);
            target.update_sink(previous.buffer, state.buffer);
        }

        void release(ReleaseContext<AudioDeviceSink> const& ctx) const
        {
            if (!ctx.execution_targets) {
                return;
            }

            auto& state = ctx.state();
            auto& target = ctx.execution_targets->audio_device(device_id, channel);
            target.unregister_sink(state.buffer);
        }

        void tick(TickSampleContext<AudioDeviceSink> const& ctx) const
        {
            auto& sink_state = ctx.state();
            if (sink_state.buffer.empty()) {
                return;
            }

            sink_state.buffer[ctx.index & (sink_state.buffer.size() - 1)] += ctx.inputs[0].get();
        }
    };
}
