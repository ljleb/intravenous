#pragma once

#include "node.h"

#include <array>
#include <string>
#include <utility>

namespace iv {
    struct SharedAccumulatingSink {
        std::string resource_id;

        struct State {
            std::span<Sample> buffer;
        };

        constexpr auto inputs() const
        {
            return std::array{
                InputConfig{ "in" }
            };
        }

        template<class Allocator>
        void init_buffer(Allocator& alloc, InitBufferContext& context) const
        {
            State& state = alloc.template new_object<State>();
            auto buffer = context.template use_tick_buffer<Sample>(resource_id);
            alloc.assign(state.buffer, buffer);
        }

        void tick(TickState const& state) const
        {
            auto& sink_state = state.get_state<State>();
            if (sink_state.buffer.empty()) {
                return;
            }

            sink_state.buffer[state.index & (sink_state.buffer.size() - 1)] += state.inputs[0].get();
        }
    };
}
