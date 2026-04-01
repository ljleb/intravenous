#pragma once

#include "node_lifecycle.h"
#include <array>

namespace iv {
    class Latency {
        size_t _latency;

    public:
        struct State {
            std::span<Sample> memory;
        };

        constexpr explicit Latency(size_t latency = 1)
        : _latency(latency)
        {}

        constexpr auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void declare(DeclarationContext<Latency> const& ctx) const
        {
            auto const& state = ctx.state();
            size_t const min_size = _latency + ctx.max_block_size();
            ctx.local_array(state.memory, next_power_of_2(min_size));
        }

        void initialize(InitializationContext<Latency> const& ctx) const
        {
            auto& state = ctx.state();
            std::ranges::fill(state.memory, Sample{});
        }

        void tick(TickSampleContext<Latency> const& ctx) const
        {
            auto& state = ctx.state();
            size_t const mask = state.memory.size() - 1;

            state.memory[(ctx.index + _latency) & mask] = ctx.inputs[0].get();
            ctx.outputs[0].push(state.memory[ctx.index & mask]);
        }
    };
}
