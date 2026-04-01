#pragma once

#include "node_lifecycle.h"
#include "math/polyblep.h"
#include <array>
#include <cmath>

namespace iv {
    struct Warper {
        auto inputs() const
        {
            return std::array {
                InputConfig { .name = "in" },
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

            Sample threshold = in_threshold.get();
            Sample sample_prev = std::fmin(std::fmax(out_aliased.get(), -1.0f), 1.0f);
            Sample sample = std::fmin(std::fmax(in.get(), -1.0f), 1.0f);
            Sample sample_warped = sample;
            bool warped = false;

            if (sample > threshold) {
                sample_warped = warp_pm1(sample, threshold);
                warped = true;
            } else if (sample < -threshold) {
                sample_warped = warp_pm1(sample, threshold);
                warped = true;
            }
            sample_warped = std::fmin(std::fmax(sample_warped, -1.0f), 1.0f);
            out_aliased.push(sample_warped);

            Sample sample_warped_aa = sample_warped;
            if (warped) {
                Sample delta = Sample((sample - sample_prev) / 2.0);
                out.update(sample_prev - polyblep_error(sample_prev, delta, threshold, PolyblepSide::LEFT));
                sample_warped_aa -= polyblep_error(sample_warped_aa, delta, threshold, PolyblepSide::RIGHT);
            }
            out.push(std::fmin(std::fmax(sample_warped_aa, -1.0f), 1.0f));
        }
    };

    struct Integrator {
        auto inputs() const
        {
            return std::array {
                InputConfig { "f_prev" },
                InputConfig { "f" },
                InputConfig { .name = "dt", .default_value = 1.0 },
            };
        }

        auto outputs() const
        {
            return std::array { OutputConfig { "integral" } };
        }

        void tick(TickSampleContext<Integrator> const& state) const
        {
            auto const f_prev = state.inputs[0].get();
            auto const f = state.inputs[1].get();
            auto const dx = state.inputs[2].get();
            state.outputs[0].push(warp_pm1(f_prev + f * dx, 1.0));
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
