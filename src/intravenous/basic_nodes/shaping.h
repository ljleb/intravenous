#pragma once

#include <intravenous/node/lifecycle.h>
#include <intravenous/math/polyblep.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace iv {
    struct Warper {
        static constexpr auto inputs()
        {
            return std::array {
                InputConfig { .name = "in", .history = 1 },
                InputConfig { .name = "threshold", .default_value = 1.0 },
            };
        }

        static constexpr auto outputs()
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

    struct PhaseIntegrator {
        static constexpr auto inputs()
        {
            return std::array { InputConfig { .name = "delta" } };
        }

        static constexpr auto outputs()
        {
            return std::array { OutputConfig { .name = "phase", .history = 1 } };
        }

        void tick_block(TickBlockContext<PhaseIntegrator> const& ctx) const
        {
            auto const delta = ctx.inputs[0].get_block(ctx.block_size);
            auto& out = ctx.outputs[0];

            Sample phase = out.get();
            for (size_t i = 0; i < ctx.block_size; ++i) {
                phase = warp_pm1(phase + delta[i], 1.0f);
                out.push(phase);
            }
        }
    };

    struct SawOscillator {
        struct State {
            Sample phase = 0.0f;
        };

        static constexpr auto inputs()
        {
            return std::array {
                InputConfig { .name = "phase_offset", .history = 1 },
                InputConfig { .name = "frequency", .history = 1, .min = 0 },
            };
        }

        static constexpr auto outputs()
        {
            return std::array {
                OutputConfig { .name = "out", .latency = 1 },
            };
        }

        void tick(TickSampleContext<SawOscillator> const& ctx) const
        {
            auto& state = ctx.state();
            auto const phi = ctx.inputs[0].get();
            auto const f = ctx.inputs[1].get();
            auto const dt = ctx.sample_period();
            auto& out = ctx.outputs[0];

            auto const phase_advance = f * 2 * dt;
            auto const x0 = state.phase + phase_advance + phi;
            auto const prev_phase_advance = ctx.inputs[1].get(1) * 2 * dt;
            auto const prev_base_phase = warp_pm1(state.phase - prev_phase_advance, 1);
            auto const x1 = prev_base_phase + prev_phase_advance + ctx.inputs[0].get(1);
            auto const prev_output = out.get();
            bool const was_inside = x1 >= -1 && x1 <= 1;
            bool const is_outside = x0 < -1 || x0 > 1;
            bool const needs_blep = was_inside && is_outside;

            auto const y0 = warp_pm1(x0, 1);

            auto y0_aa = y0;
            if (needs_blep) {
                auto const delta = (y0 - prev_output) / 2.0f;
                auto const left = prev_output - polyblep_error(prev_output, delta, 1, PolyblepSide::LEFT);
                out.update(left);
                y0_aa -= polyblep_error(y0_aa, delta, 1, PolyblepSide::RIGHT);
            }

            out.push(y0_aa);
            state.phase = warp_pm1(state.phase + phase_advance, 1);
        }
    };

    struct SineOscillator {
        static constexpr auto inputs()
        {
            return std::array {
                InputConfig { .name = "phase_offset", .history = 1 },
                InputConfig { .name = "frequency", .history = 1, .min = 0 },
            };
        }

        static constexpr auto outputs()
        {
            return std::array {
                OutputConfig { .name = "out", .latency = 1 },
            };
        }

        struct State {
            double anchor_phase = 0.0;
            size_t anchor_index = 0;
            double frequency = 0.0;
            bool initialized = false;
        };

        static constexpr size_t table_size = 4096;
        static constexpr std::array<Sample, table_size + 1> table = [] {
            std::array<Sample, table_size + 1> table{};

            for (size_t i = 0; i < table_size; ++i) {
                double const phase =
                    -1.0 + 2.0 * static_cast<double>(i) / static_cast<double>(table_size);

                table[i] = static_cast<Sample>(
                    std::sin(phase * std::numbers::pi)
                );
            }

            // Exact periodic endpoint, rather than evaluating sin(pi) separately.
            table[table_size] = table[0];

            return table;
        }();

        static Sample sine(double phase)
        {
            phase = wrap(phase);

            constexpr double scale =
                static_cast<double>(table_size) * 0.5;

            double const position = (phase + 1.0) * scale;

            size_t const index =
                static_cast<size_t>(position);

            double const alpha =
                position - static_cast<double>(index);

            double const a = table[index];
            double const b = table[index + 1];

            return static_cast<Sample>(
                a + alpha * (b - a)
            );
        }

        static double wrap(double x)
        {
            return x - std::floor((x + 1.0) / 2.0) * 2.0;
        }

        void tick(TickSampleContext<SineOscillator> const& ctx) const
        {
            auto& state = ctx.state();

            auto const f = ctx.inputs[1].get();
            auto const phi = ctx.inputs[0].get();
            auto const dt = ctx.sample_period();

            if (!state.initialized) {
                state.frequency = f;
                state.anchor_index = ctx.index;
                state.anchor_phase = 0.0;
                state.initialized = true;
            }

            // Re-anchor if frequency changed.
            if (f != state.frequency) {
                double const elapsed =
                    static_cast<double>(ctx.index - state.anchor_index);

                state.anchor_phase = wrap(
                    state.anchor_phase +
                    state.frequency * 2.0 * elapsed * dt
                );

                state.anchor_index = ctx.index;
                state.frequency = f;
            }

            auto const elapsed =
                static_cast<double>(ctx.index - state.anchor_index);

            auto const phase =
                state.anchor_phase +
                state.frequency * 2.0 * (elapsed + 1.0) * dt;

            auto const y =
                sine((phase + phi) * std::numbers::pi);

            ctx.outputs[0].push(y);
        }
    };

    struct PhaseOffsetPredictor {
        size_t _latency;

        explicit PhaseOffsetPredictor(size_t latency) :
            _latency(latency)
        {}

        static constexpr auto inputs()
        {
            return std::array { InputConfig { "f" } };
        }

        static constexpr auto outputs()
        {
            return std::array { OutputConfig { "offset" } };
        }

        void tick(TickSampleContext<PhaseOffsetPredictor> const& state) const
        {
            auto const f = state.inputs[0].get();
            Sample const extra_latency = _latency > 0 ? (_latency - 1) : 0;
            state.outputs[0].push(f * state.sample_period() * extra_latency);
        }
    };

    struct Interpolation {
        static constexpr auto inputs()
        {
            return std::array {
                InputConfig { "a" },
                InputConfig { "b" },
                InputConfig { "alpha" },
            };
        }

        static constexpr auto outputs()
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
