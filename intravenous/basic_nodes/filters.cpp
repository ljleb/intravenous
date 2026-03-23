#include "filters.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace iv {
    void WhackIirThing::tick(TickState const& state) const
    {
        Sample in_dry = state.inputs[0].get();
        Sample in_control = state.inputs[1].get();
        Sample alpha = 1 - in_control;
        for (size_t i = 0; i < 4; ++i) {
            alpha *= alpha;
        }

        auto& out_low = state.outputs[0];
        auto& out_high = state.outputs[1];
        Sample last_low = out_low.get();
        Sample low = last_low + alpha * (in_dry - last_low);
        out_low.push(low);
        out_high.push(in_dry - low);
    }

    void SimpleIirHighPass::tick(TickState const& state) const
    {
        auto& in = state.inputs[0];
        auto const ctrl = state.inputs[1].get();
        auto const dx = state.inputs[2].get();
        auto& out = state.outputs[0];

        auto const usableMax = std::min<Sample>(FMAX, 0.5f / dx);
        auto const f_c = FMIN * std::pow(usableMax / FMIN, ctrl);
        auto const norm = f_c * dx;
        auto const c = std::tanf(std::numbers::pi_v<Sample> * norm);
        auto const a1 = (1.0f - c) / (1.0f + c);
        auto const b0 = 1.0f / (1.0f + c);

        auto const x = in.get(0);
        auto const x1 = in.get(1);
        auto const y1 = out.get();
        out.push(a1 * y1 + b0 * (x - x1));
    }

    void SimpleIirLowPass::tick(TickState const& state) const
    {
        auto& in_port = state.inputs[0];
        auto const ctrl = state.inputs[1].get();
        auto const dx = state.inputs[2].get();
        auto& out = state.outputs[0];

        auto const usableMax = std::min<Sample>(FMAX, 0.5f / dx);
        auto const f_c = FMIN * std::pow(usableMax / FMIN, ctrl);
        auto const norm = f_c * dx;
        auto const c = std::tanf(std::numbers::pi_v<Sample> * norm);
        auto const a1 = (1.0f - c) / (1.0f + c);
        auto const alpha = c / (1.0f + c);

        auto const x = in_port.get(0);
        auto const x_prev = in_port.get(1);
        auto const y_prev = out.get();
        out.push(a1 * y_prev + alpha * (x + x_prev));
    }
}
