#pragma once

#include "node.h"
#include <array>
#include <numbers>

namespace iv {
    struct WhackIirThing {
        auto inputs() const
        {
            return std::array<InputConfig, 3>{};
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 2>{};
        }

        void tick(TickContext<WhackIirThing> const& ctx) const
        {
            Sample in_dry = ctx.inputs[0].get();
            Sample in_control = ctx.inputs[1].get();
            Sample alpha = 1 - in_control;
            for (size_t i = 0; i < 4; ++i) {
                alpha *= alpha;
            }

            auto& out_low = ctx.outputs[0];
            auto& out_high = ctx.outputs[1];
            Sample last_low = out_low.get();
            Sample low = last_low + alpha * (in_dry - last_low);
            out_low.push(low);
            out_high.push(in_dry - low);
        }
    };

    struct SimpleIirHighPass {
        static constexpr Sample FMIN = 2e1f;
        static constexpr Sample FMAX = 2e4f;

        auto inputs() const
        {
            return std::array {
                InputConfig { .name = "in", .history = 1 },
                InputConfig { .name = "cutoff" },
                InputConfig { .name = "dx", .default_value = 1.0 },
            };
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickContext<SimpleIirHighPass> const& ctx) const
        {
            auto& in = ctx.inputs[0];
            auto const ctrl = ctx.inputs[1].get();
            auto const dx = ctx.inputs[2].get();
            auto& out = ctx.outputs[0];

            auto const usableMax = std::min<Sample>(FMAX, 0.5f / dx);
            auto const f_c = FMIN * std::pow(usableMax / FMIN, ctrl);
            auto const norm = f_c * dx;
            auto const c = std::tanf(std::numbers::pi_v<Sample::storage> * norm);
            auto const a1 = (1.0f - c) / (1.0f + c);
            auto const b0 = 1.0f / (1.0f + c);

            auto const x = in.get(0);
            auto const x1 = in.get(1);
            auto const y1 = out.get();
            out.push(a1 * y1 + b0 * (x - x1));
        }
    };

    struct SimpleIirLowPass {
        static constexpr Sample FMIN = 2e1f;
        static constexpr Sample FMAX = 2e4f;

        auto inputs() const
        {
            return std::array {
                InputConfig { .name = "in", .history = 1 },
                InputConfig { .name = "cutoff" },
                InputConfig { .name = "dx", .default_value = 1.0 },
            };
        }

        auto outputs() const
        {
            return std::array { OutputConfig { "out" } };
        }

        void tick(TickContext<SimpleIirLowPass> const& ctx) const
        {
            auto& in_port = ctx.inputs[0];
            auto const ctrl = ctx.inputs[1].get();
            auto const dx = ctx.inputs[2].get();
            auto& out = ctx.outputs[0];

            auto const usableMax = std::min<Sample>(FMAX, 0.5f / dx);
            auto const f_c = FMIN * std::pow(usableMax / FMIN, ctrl);
            auto const norm = f_c * dx;
            auto const c = std::tanf(std::numbers::pi_v<Sample::storage> * norm);
            auto const a1 = (1.0f - c) / (1.0f + c);
            auto const alpha = c / (1.0f + c);

            auto const x = in_port.get(0);
            auto const x_prev = in_port.get(1);
            auto const y_prev = out.get();
            out.push(a1 * y_prev + alpha * (x + x_prev));
        }
    };
}
