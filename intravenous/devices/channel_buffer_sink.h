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

        auto inputs() const
        {
            return std::array{
                InputConfig{ "in" }
            };
        }

        void declare(DeclarationContext<SharedAccumulatingSink> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.import_array(resource_id, state.buffer);
        }

        void tick(TickContext<SharedAccumulatingSink> const& ctx) const
        {
            auto& sink_state = ctx.state();
            if (sink_state.buffer.empty()) {
                return;
            }

            sink_state.buffer[ctx.index & (sink_state.buffer.size() - 1)] += ctx.inputs[0].get();
        }
    };
}
