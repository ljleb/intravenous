#pragma once

#include "node.h"
#include <array>

namespace iv {
    class Latency {
        size_t _latency;

    public:
        constexpr explicit Latency(size_t latency = 1) :
            _latency(latency)
        {}

        constexpr auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { .latency = _latency } };
        }

        void tick(TickState const& state)
        {
            state.outputs[0].push(state.inputs[0].get());
        }
    };
}
