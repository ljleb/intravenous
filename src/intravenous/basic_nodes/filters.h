#pragma once

#include <intravenous/node/lifecycle.h>
#include <intravenous/node/port_constraints.h>
#include <array>
#include <numbers>

namespace iv {
    struct WhackIirThing {
        static constexpr auto inputs()
        {
            return std::array<SequentialInputConfig<Sample>, 3>{};
        }

        static constexpr auto outputs()
        {
            return std::array<TickOutputConfig<Sample>, 2>{};
        }

        void tick(TickSampleContext<WhackIirThing> const& ctx) const
        {
            Sample in_dry = ctx.template input<0>().get();
            Sample in_control = ctx.template input<1>().get();
            Sample alpha = 1 - in_control;
            for (size_t i = 0; i < 4; ++i) {
                alpha *= alpha;
            }

            auto out_low = ctx.template output<0>();
            auto out_high = ctx.template output<1>();
            Sample last_low = out_low.get();
            Sample low = last_low + alpha * (in_dry - last_low);
            out_low.push(low);
            out_high.push(in_dry - low);
        }
    };

    struct SimpleIirHighPass {
        static constexpr Sample minimum_cutoff_hz = 20.0f;
        static constexpr Sample maximum_cutoff_hz = 20'000.0f;

        static constexpr auto inputs()
        {
            return std::array {
                input<Sample>("in"),
                input<Sample>("cutoff", {
                    .default_value = 1.0f,
                    .min = 0.0f,
                    .max = 1.0f,
                }),
            };
        }

        void constrain_ports(ConstrainPortsContext<SimpleIirHighPass>& ctx) const
        {
            ctx.input<"in">().history() = 1;
        }

        static constexpr auto outputs()
        {
            return std::array { output<Sample>("out") };
        }

        void tick(TickSampleContext<SimpleIirHighPass> const& ctx) const
        {
            auto in = ctx.input<"in">();
            auto const ctrl = ctx.input<"cutoff">().get();
            auto const sample_period = ctx.sample_period();
            auto out = ctx.output<"out">();

            auto const maximum_usable_cutoff_hz = std::min<Sample>(
                maximum_cutoff_hz, 0.5f / sample_period);
            auto const cutoff_hz = minimum_cutoff_hz * std::pow(
                maximum_usable_cutoff_hz / minimum_cutoff_hz, ctrl);
            auto const normalized_cutoff = cutoff_hz * sample_period;
            auto const c = std::tanf(
                std::numbers::pi_v<Sample::storage> * normalized_cutoff);
            auto const a1 = (1.0f - c) / (1.0f + c);
            auto const b0 = 1.0f / (1.0f + c);

            auto const x = in.get(0);
            auto const x1 = in.get(1);
            auto const y1 = out.get();
            out.push(a1 * y1 + b0 * (x - x1));
        }
    };

    struct SimpleIirLowPass {
        static constexpr Sample minimum_cutoff_hz = 20.0f;
        static constexpr Sample maximum_cutoff_hz = 20'000.0f;

        static constexpr auto inputs()
        {
            return std::array {
                input<Sample>("in"),
                input<Sample>("cutoff", {
                    .default_value = 1.0f,
                    .min = 0.0f,
                    .max = 1.0f,
                }),
            };
        }

        void constrain_ports(ConstrainPortsContext<SimpleIirLowPass>& ctx) const
        {
            ctx.input<"in">().history() = 1;
        }

        static constexpr auto outputs()
        {
            return std::array { output<Sample>("out") };
        }

        void tick(TickSampleContext<SimpleIirLowPass> const& ctx) const
        {
            auto in_port = ctx.input<"in">();
            auto const ctrl = ctx.input<"cutoff">().get();
            auto const sample_period = ctx.sample_period();
            auto out = ctx.output<"out">();

            auto const maximum_usable_cutoff_hz = std::min<Sample>(
                maximum_cutoff_hz, 0.5f / sample_period);
            auto const cutoff_hz = minimum_cutoff_hz * std::pow(
                maximum_usable_cutoff_hz / minimum_cutoff_hz, ctrl);
            auto const normalized_cutoff = cutoff_hz * sample_period;
            auto const c = std::tanf(
                std::numbers::pi_v<Sample::storage> * normalized_cutoff);
            auto const a1 = (1.0f - c) / (1.0f + c);
            auto const alpha = c / (1.0f + c);

            auto const x = in_port.get(0);
            auto const x_prev = in_port.get(1);
            auto const y_prev = out.get();
            out.push(a1 * y_prev + alpha * (x + x_prev));
        }
    };
}
