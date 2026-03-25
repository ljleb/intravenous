#include "shaping.h"
#include "math/polyblep.h"
#include <cmath>

namespace iv {
    void Warper::tick(TickState const& state)
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

    void Integrator::tick(TickState const& state)
    {
        auto const f_prev = state.inputs[0].get();
        auto const f = state.inputs[1].get();
        auto const dx = state.inputs[2].get();
        state.outputs[0].push(warp_pm1(f_prev + f * dx, 1.0));
    }

    void Interpolation::tick(TickState const& state)
    {
        Sample a = state.inputs[0].get();
        Sample b = state.inputs[1].get();
        Sample alpha = state.inputs[2].get();
        state.outputs[0].push(a + alpha * (b - a));
    }
}
