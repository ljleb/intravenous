#pragma once

#include "node_lifecycle.h"
#include "math/polyblep.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace iv {
    struct Warper {
        auto inputs() const
        {
            return std::array {
                InputConfig { .name = "in", .history = 1 },
                InputConfig { .name = "threshold", .default_value = 1.0 },
            };
        }

        auto outputs() const
        {
            return std::array {
                OutputConfig { .name = "anti_aliased", .latency = 1 },
                OutputConfig { .name = "aliased" },
            };
        }

        void tick(TickSampleContext<Warper> const& state) const
        {
            auto const& in = state.inputs[0];
            auto const& in_threshold = state.inputs[1];
            auto& out = state.outputs[0];
            auto& out_aliased = state.outputs[1];

            auto const threshold = in_threshold.get();
            auto const x0 = in.get();
            auto const x1 = in.get(1);
            auto const y1 = out.get();

            bool const was_inside = x1 >= -threshold && x1 <= threshold;
            bool const is_outside = x0 < -threshold || x0 > threshold;
            bool const needs_blep = was_inside && is_outside;

            auto const y2 = warp_pm1(x0, threshold);
            out_aliased.push(y2);
            auto y2_aa = y2;
            if (needs_blep) {
                auto delta = (y2 - y1) / 2.0;
                out.update(y1 - polyblep_error(y1, delta, threshold, PolyblepSide::LEFT));
                y2_aa -= polyblep_error(y2_aa, delta, threshold, PolyblepSide::RIGHT);
            }
            out.push(y2_aa);
        }
    };

    struct Integrator {
        struct State {
            std::span<Sample> global_offsets;
        };

        auto inputs() const
        {
            return std::array {
                InputConfig { .name = "f_prev", .history = 1 },
                InputConfig { "f" },
                InputConfig { .name = "dt", .default_value = 1.0 },
            };
        }

        auto outputs() const
        {
            return std::array { OutputConfig { .name = "integral", .history = 1 } };
        }

        void declare(DeclarationContext<Integrator> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.global_offsets, ctx.max_block_size());
        }

        void initialize(InitializationContext<Integrator> const& ctx) const
        {
            auto& state = ctx.state();
            std::fill(state.global_offsets.begin(), state.global_offsets.end(), Sample(0.0f));
        }

        void tick_block(TickBlockContext<Integrator> const& ctx) const
        {
            auto& state = ctx.state();
            auto const f_prev = ctx.inputs[0].get_block(ctx.block_size);
            auto const f = ctx.inputs[1].get_block(ctx.block_size);
            auto const dt = ctx.inputs[2].get_block(ctx.block_size);
            auto& out = ctx.outputs[0];

            if (ctx.block_size == 0) {
                return;
            }

            Sample const decay = 0.0;
            Sample target_offset = state.global_offsets[ctx.block_size - 1];
            for (size_t i = 0; i < ctx.block_size; ++i) {
                auto const integrand = f[i] * dt[i] + (f_prev[i] - out.get()) * decay;
                target_offset += integrand;
                auto const offset_delta = target_offset - state.global_offsets[i];
                out.push(f_prev[i] + offset_delta);
                state.global_offsets[i] = target_offset;
            }
        }
    };

    struct PhaseOffsetPredictor {
        size_t _latency;

        explicit PhaseOffsetPredictor(size_t latency) :
            _latency(latency)
        {}

        auto inputs() const
        {
            return std::array {
                InputConfig { "f" },
                InputConfig { .name = "dt", .default_value = 1.0 },
            };
        }

        auto outputs() const
        {
            return std::array { OutputConfig { "offset" } };
        }

        void tick(TickSampleContext<PhaseOffsetPredictor> const& state) const
        {
            auto const f = state.inputs[0].get();
            auto const dt = state.inputs[1].get();
            Sample const extra_latency = _latency > 0 ? (_latency - 1) : 0;
            state.outputs[0].push(f * dt * extra_latency);
        }
    };

    struct Interpolation {
        auto inputs() const
        {
            return std::array {
                InputConfig { "a" },
                InputConfig { "b" },
                InputConfig { "alpha" },
            };
        }

        auto outputs() const
        {
            return std::array { OutputConfig { "out" } };
        }

        void tick(TickSampleContext<Interpolation> const& state) const
        {
            Sample a = state.inputs[0].get();
            Sample b = state.inputs[1].get();
            Sample alpha = state.inputs[2].get();
            state.outputs[0].push(a + alpha * (b - a));
        }
    };
}
