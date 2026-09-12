#include <intravenous/dsl.h>

#include "P0014_bspline_runtime_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <string>

using namespace iv;

struct LearnedHrtfSource
{
    static constexpr size_t latent_parameter_count = 24;
    static constexpr size_t pinna_filter_count = 8;
    static constexpr size_t input_history = 63;

    static constexpr float speed_of_sound = 343.0f;
    static constexpr float head_radius = 0.0875f;
    static constexpr float inverse_degrees_per_turn = 1.0f / 360.0f;

    static constexpr auto inputs()
    {
        return std::array<InputConfig, 3> {
            InputConfig { .name = "in", .history = input_history, },
            InputConfig { .name = "azimuth", .default_value = 0, .min = -180, .max = 180, },
            InputConfig { .name = "elevation", .default_value = 0, .min = -180, .max = 180, },
        };
    }

    static constexpr auto outputs()
    {
        return std::array<OutputConfig, 1> {
            OutputConfig { .name = "out", .channel_layout = { .channel_type = ChannelTypeId::stereo, } },
        };
    }

    struct BiquadCoefficients
    {
        float b0 = 1.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        float a1 = 0.0f;
        float a2 = 0.0f;
    };

    struct BiquadState
    {
        float s1 = 0.0f;
        float s2 = 0.0f;
    };

    struct HeadCoefficients
    {
        float b0 = 1.0f;
        float b1 = 0.0f;
    };

    struct HeadState
    {
        float previous_input = 0.0f;
        float previous_output = 0.0f;
    };

    struct EarParameters
    {
        std::array<float, 4> peak_frequency {};
        std::array<float, 4> notch_frequency {};

        std::array<float, 4> peak_gain {};
        std::array<float, 4> notch_gain {};

        std::array<float, 4> peak_q {};
        std::array<float, 4> notch_q {};
    };

    struct EarRuntime
    {
        HeadCoefficients head {};
        HeadState head_state {};

        std::array<BiquadCoefficients, pinna_filter_count> filters {};
        std::array<BiquadState, pinna_filter_count> filter_state {};

        float delay_samples = 0.0f;
    };

    struct CubicLevel
    {
        std::uint16_t segment = 0;
        std::array<float, 4> weight {};
    };

    struct QuadraticLevel
    {
        std::uint16_t segment = 0;
        std::array<float, 3> weight {};
    };

    struct State
    {
        float azimuth = 0.0f;
        float elevation = 0.0f;

        float sample_rate = 48000.0f;

        float head_omega0 = 0.0f;
        float head_inverse_denominator = 0.0f;
        float head_feedback = 0.0f;

        float radius_over_c_samples = 0.0f;

        std::array<EarRuntime, 2> ear {};
    };

    void initialize(InitializationContext<LearnedHrtfSource> const& ctx) const
    {
        auto& state = ctx.state();

        state.head_omega0 = speed_of_sound / head_radius;
        state.head_inverse_denominator = 1.0f / (state.head_omega0 + state.sample_rate);
        state.head_feedback = (state.sample_rate - state.head_omega0) * state.head_inverse_denominator;
        state.radius_over_c_samples = head_radius * state.sample_rate / speed_of_sound;

        update_direction(state, 0.0f, 0.0f);
    }

    void tick_block(TickBlockContext<LearnedHrtfSource> const& ctx) const
    {
        auto& state = ctx.state();
        auto const& input = ctx.inputs[0];
        auto const azimuth = ctx.input<"azimuth">();
        auto const elevation = ctx.input<"elevation">();
        auto output = ctx.output<"out">();

        for (size_t i = 0; i < ctx.block_size; ++i) {
            auto const current_azimuth = static_cast<float>(azimuth[i]);
            auto const current_elevation = static_cast<float>(elevation[i]);

            if (current_azimuth != state.azimuth || current_elevation != state.elevation)
                update_direction(state, current_azimuth, current_elevation);

            for (size_t channel = 0; channel < 2; ++channel) {
                auto& ear = state.ear[channel];

                auto value = read_fractional_delay(input, i, ear.delay_samples);
                value = process_head(ear, state.head_feedback, value);

                for (size_t filter = 0; filter < pinna_filter_count; ++filter)
                    value = process_biquad(ear.filters[filter], ear.filter_state[filter], value);

                if (channel == 0)
                    output[stereo::left][i] = value;
                else
                    output[stereo::right][i] = value;
            }
        }
    }

private:
    static Sample read_ago(InputPort const& input, size_t frame, size_t samples_ago)
    {
        if (samples_ago <= frame)
            return input.get_frame(frame - samples_ago);

        return input.get(samples_ago - frame);
    }

    static Sample read_fractional_delay(InputPort const& input, size_t frame, float delay)
    {
        auto const whole = static_cast<size_t>(delay);
        auto const fraction = delay - static_cast<float>(whole);
        auto const a = read_ago(input, frame, whole);
        auto const b = read_ago(input, frame, whole + 1);
        return a + fraction * (b - a);
    }

    static float softplus(float x)
    {
        return std::log1p(std::exp(x));
    }

    static float sigmoid(float x)
    {
        return 1.0f / (1.0f + std::exp(-x));
    }

    static CubicLevel cubic_level(float u, size_t level)
    {
        constexpr float one_sixth = 1.0f / 6.0f;

        auto const knot_count =
            static_cast<std::uint16_t>(p0014_bspline_model::u_base_knot_count << level);

        auto const position = u * static_cast<float>(knot_count);
        auto const segment = static_cast<std::uint16_t>(position);
        auto const t = position - static_cast<float>(segment);
        auto const t2 = t * t;
        auto const t3 = t2 * t;
        auto const one_minus_t = 1.0f - t;
        auto const one_minus_t2 = one_minus_t * one_minus_t;

        return {
            segment,
            {
                one_minus_t2 * one_minus_t * one_sixth,
                (3.0f * t3 - 6.0f * t2 + 4.0f) * one_sixth,
                (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) * one_sixth,
                t3 * one_sixth,
            },
        };
    }

    static QuadraticLevel quadratic_level(float v, size_t level)
    {
        auto const segment_count =
            static_cast<std::uint16_t>(p0014_bspline_model::v_base_segment_count << level);

        auto const position = v * static_cast<float>(segment_count);
        auto segment = static_cast<std::uint16_t>(position);

        // v == 1 belongs to the final open-knot segment with local coordinate 1.
        if (segment == segment_count)
            --segment;

        auto const t = position - static_cast<float>(segment);
        auto const t2 = t * t;
        auto const one_minus_t = 1.0f - t;
        auto const one_minus_t2 = one_minus_t * one_minus_t;

        QuadraticLevel result;
        result.segment = segment;

        if (segment == 0) {
            result.weight[0] = one_minus_t2;
            result.weight[1] = 2.0f * t - 1.5f * t2;
            result.weight[2] = 0.5f * t2;
        }
        else if (segment + 1 == segment_count) {
            result.weight[0] = 0.5f * one_minus_t2;
            result.weight[1] = 2.0f * one_minus_t - 1.5f * one_minus_t2;
            result.weight[2] = t2;
        }
        else {
            result.weight[0] = 0.5f * one_minus_t2;
            result.weight[1] = 0.5f + t - t2;
            result.weight[2] = 0.5f * t2;
        }

        return result;
    }

    static std::array<float, latent_parameter_count> evaluate_latent(float u, float v)
    {
        using namespace p0014_bspline_model;

        std::array<CubicLevel, u_max_level + 1> u_level {};
        std::array<QuadraticLevel, v_max_level + 1> v_level {};

        for (size_t level = 0; level <= u_max_level; ++level)
            u_level[level] = cubic_level(u, level);

        for (size_t level = 0; level <= v_max_level; ++level)
            v_level[level] = quadratic_level(v, level);

        auto const dispatch_u = static_cast<size_t>(u * static_cast<float>(dispatch_u_count));
        auto dispatch_v = static_cast<size_t>(v * static_cast<float>(dispatch_v_count));

        // The open vertical spline includes v == 1 exactly.
        if (dispatch_v == dispatch_v_count)
            --dispatch_v;

        auto const cell = dispatch_v * dispatch_u_count + dispatch_u;
        auto const first = cell_offset[cell];
        auto const count = cell_count[cell];

        std::array<float, latent_parameter_count> result {};

        for (size_t candidate = 0; candidate < count; ++candidate) {
            auto const basis = cell_basis[first + candidate];

            auto const lu = basis_u_level[basis];
            auto const lv = basis_v_level[basis];

            auto const u_knot_count =
                static_cast<std::uint16_t>(u_base_knot_count << lu);

            // Periodic cubic support: basis indices segment-1 .. segment+2.
            auto const u_slot = static_cast<std::uint16_t>(
                basis_u_index[basis] - u_level[lu].segment + 1)
                & static_cast<std::uint16_t>(u_knot_count - 1);

            if (u_slot >= 4)
                continue;

            auto const v_slot =
                static_cast<int>(basis_v_index[basis])
                - static_cast<int>(v_level[lv].segment);

            if (v_slot < 0 || v_slot >= 3)
                continue;

            auto const weight =
                u_level[lu].weight[u_slot]
                * v_level[lv].weight[static_cast<size_t>(v_slot)];

            auto const term_first = basis_term_offset[basis];
            auto const term_count = basis_term_count[basis];

            for (size_t term = 0; term < term_count; ++term) {
                auto const index = term_first + term;
                result[term_parameter[index]] += weight * term_coefficient[index];
            }
        }

        return result;
    }

    static void decode_frequency_group(float const* latent, std::array<float, 4>& frequency)
    {
        constexpr float minimum_frequency = 1500.0f;
        constexpr float maximum_frequency = 22000.0f;

        float const log_frequency_range = std::log(maximum_frequency / minimum_frequency);

        std::array<float, 4> gap {
            softplus(latent[0]),
            softplus(latent[1]),
            softplus(latent[2]),
            softplus(latent[3]),
        };

        auto const inverse_total = 1.0f / (1.0f + gap[0] + gap[1] + gap[2] + gap[3]);

        float cumulative = 0.0f;
        for (size_t i = 0; i < 4; ++i) {
            cumulative += gap[i];
            frequency[i] = minimum_frequency * std::exp(log_frequency_range * cumulative * inverse_total);
        }
    }

    static EarParameters decode(std::array<float, latent_parameter_count> const& latent)
    {
        constexpr float maximum_gain_db = 30.0f;
        constexpr float minimum_q = 0.3f;
        constexpr float q_range = 20.0f - minimum_q;

        EarParameters result;

        decode_frequency_group(latent.data(), result.peak_frequency);
        decode_frequency_group(latent.data() + 4, result.notch_frequency);

        for (size_t i = 0; i < 4; ++i) {
            result.peak_gain[i] = maximum_gain_db * sigmoid(latent[8 + i]);
            result.notch_gain[i] = -maximum_gain_db * sigmoid(latent[12 + i]);
            result.peak_q[i] = minimum_q + q_range * sigmoid(latent[16 + i]);
            result.notch_q[i] = minimum_q + q_range * sigmoid(latent[20 + i]);
        }

        return result;
    }

    static BiquadCoefficients peaking_eq(float frequency, float gain_db, float q, float sample_rate)
    {
        auto const A = std::exp(gain_db * std::numbers::ln10_v<float> / 40.0f);
        auto const omega = 2.0f * std::numbers::pi_v<float> * frequency / sample_rate;
        auto const sin_omega = std::sin(omega);
        auto const cos_omega = std::cos(omega);
        auto const alpha = sin_omega / (2.0f * q);
        auto const b0 = 1.0f + alpha * A;
        auto const b1 = -2.0f * cos_omega;
        auto const b2 = 1.0f - alpha * A;
        auto const a0 = 1.0f + alpha / A;
        auto const a1 = -2.0f * cos_omega;
        auto const a2 = 1.0f - alpha / A;
        auto const inverse_a0 = 1.0f / a0;

        return {
            b0 * inverse_a0,
            b1 * inverse_a0,
            b2 * inverse_a0,
            a1 * inverse_a0,
            a2 * inverse_a0,
        };
    }

    static float process_biquad(BiquadCoefficients const& coefficients, BiquadState& state, float input)
    {
        auto const output = coefficients.b0 * input + state.s1;
        state.s1 = coefficients.b1 * input - coefficients.a1 * output + state.s2;
        state.s2 = coefficients.b2 * input - coefficients.a2 * output;
        return output;
    }

    static float process_head(EarRuntime& ear, float feedback, float input)
    {
        auto& state = ear.head_state;
        auto const output =
            ear.head.b0 * input
            + ear.head.b1 * state.previous_input
            + feedback * state.previous_output;

        state.previous_input = input;
        state.previous_output = output;

        return output;
    }

    static void update_ear(State& state, size_t channel, float u, float v, float y)
    {
        auto const latent = evaluate_latent(u, v);
        auto const parameters = decode(latent);

        auto& ear = state.ear[channel];

        constexpr float alpha_min = 0.1f;
        constexpr float theta_min = 150.0f * std::numbers::pi_v<float> / 180.0f;
        auto const theta = std::acos(y);
        auto const head_theta = std::min(theta, theta_min);
        auto const alpha =
            (1.0f + alpha_min * 0.5f)
            + (1.0f - alpha_min * 0.5f)
                * std::cos(std::numbers::pi_v<float> * head_theta / theta_min);

        ear.head.b0 =
            (state.head_omega0 + alpha * state.sample_rate)
            * state.head_inverse_denominator;
        ear.head.b1 =
            (state.head_omega0 - alpha * state.sample_rate)
            * state.head_inverse_denominator;

        constexpr float half_pi = std::numbers::pi_v<float> / 2.0f;

        float relative_delay;
        if (theta < half_pi)
            relative_delay = -state.radius_over_c_samples * y;
        else
            relative_delay = state.radius_over_c_samples * (theta - half_pi);

        ear.delay_samples = state.radius_over_c_samples + relative_delay;

        for (size_t i = 0; i < 4; ++i) {
            ear.filters[i * 2] = peaking_eq(
                parameters.peak_frequency[i],
                parameters.peak_gain[i],
                parameters.peak_q[i],
                state.sample_rate);

            ear.filters[i * 2 + 1] = peaking_eq(
                parameters.notch_frequency[i],
                parameters.notch_gain[i],
                parameters.notch_q[i],
                state.sample_rate);
        }
    }

    static float wrap_unit(float x)
    {
        // With IV's [-180,+180] azimuth input and the optional half-turn below,
        // x is in [-0.5,1].  One correction is sufficient.
        if (x < 0.0f)
            x += 1.0f;
        if (x >= 1.0f)
            x -= 1.0f;
        return x;
    }

    static void update_direction(State& state, float azimuth, float elevation)
    {
        auto const azimuth_radians = azimuth * std::numbers::pi_v<float> / 180.0f;
        auto const elevation_radians = elevation * std::numbers::pi_v<float> / 180.0f;

        auto const sin_azimuth = std::sin(azimuth_radians);
        auto const cos_elevation = std::cos(elevation_radians);
        auto const sin_elevation = std::sin(elevation_radians);

        // Fitted chart: u = SOFA azimuth / 360 (positive toward +y / left),
        // v = (z+1)/2.  IV azimuth is positive right, hence the minus sign.
        // When cos(elevation)<0 the external double-cover represents the same
        // Cartesian direction with a canonical azimuth shifted by half a turn.
        auto const half_turn = cos_elevation < 0.0f ? 0.5f : 0.0f;
        auto const left_u = wrap_unit(-azimuth * inverse_degrees_per_turn + half_turn);

        // Mirroring y for the generic right ear mirrors azimuth around the x/z plane.
        auto right_u = 1.0f - left_u;
        if (right_u >= 1.0f)
            right_u -= 1.0f;

        auto const v = 0.5f * (sin_elevation + 1.0f);
        auto const left_y = -cos_elevation * sin_azimuth;

        update_ear(state, 0, left_u, v, left_y);
        update_ear(state, 1, right_u, v, -left_y);

        state.azimuth = azimuth;
        state.elevation = elevation;
    }
};

struct BaselineFir256
{
    static constexpr size_t tap_count = 256;
    static constexpr size_t input_history = tap_count - 1;

    static constexpr char coefficient_path[] =
        "/home/ljleb/Downloads/P0014_v2_runtime_baseline_ir_256.csv";

    static constexpr auto inputs()
    {
        return std::array<InputConfig, 1> {
            InputConfig {
                .name = "in",
                .channel_layout = { .channel_type = ChannelTypeId::stereo, },
                .history = input_history,
            },
        };
    }

    static constexpr auto outputs()
    {
        return std::array<OutputConfig, 1> {
            OutputConfig {
                .name = "out",
                .channel_layout = { .channel_type = ChannelTypeId::stereo, },
            },
        };
    }

    struct State
    {
        std::array<float, tap_count> taps {};
    };

    void initialize(InitializationContext<BaselineFir256> const& ctx) const
    {
        auto& state = ctx.state();

        std::ifstream file(coefficient_path);

        if (!file)
            throw std::runtime_error(std::string("could not open ") + coefficient_path);

        std::string line;
        std::getline(file, line);

        for (size_t i = 0; i < tap_count; ++i) {
            if (!std::getline(file, line))
                throw std::runtime_error("incomplete baseline FIR CSV");

            auto const comma = line.rfind(',');
            if (comma == std::string::npos)
                throw std::runtime_error("malformed baseline FIR CSV");

            char* end = nullptr;
            state.taps[i] = std::strtof(line.c_str() + comma + 1, &end);

            if (end == line.c_str() + comma + 1)
                throw std::runtime_error("malformed baseline FIR coefficient");
        }
    }

    void tick_block(TickBlockContext<BaselineFir256> const& ctx) const
    {
        auto const& state = ctx.state();
        auto const& input = ctx.inputs[0];
        auto output = ctx.output<"out">();

        for (size_t frame = 0; frame < ctx.block_size; ++frame) {
            auto const current_taps = std::min(frame + 1, tap_count);

            for (size_t channel = 0; channel < 2; ++channel) {
                float sum = 0.0f;

                for (size_t tap = 0; tap < current_taps; ++tap)
                    sum += state.taps[tap] * input.get_frame(frame - tap, channel);

                for (size_t tap = current_taps; tap < tap_count; ++tap)
                    sum += state.taps[tap] * input.get(tap - frame, channel);

                if (channel == 0)
                    output[stereo::left][frame] = sum;
                else
                    output[stereo::right][frame] = sum;
            }
        }
    }
};


void single_pan(GraphBuilder& g)
{
    auto const in = g.input<"in">();
    auto const az = g.input<"azimuth">(0, -180, 180);
    auto const el = g.input<"elevation">(0, -180, 180);

    auto const hrtf = g.node<LearnedHrtfSource>();

    hrtf(
        "in"_P = in,
        "azimuth"_P = az,
        "elevation"_P = el
    );

    g.outputs(hrtf);
}


void module_main(GraphBuilder& g)
{
    auto const in = g.input<"in", stereo>();
    auto const center = g.input<"azimuth">(0, -180, 180);
    auto const el = g.input<"elevation">(0, -180, 180);
    auto const spread = g.input<"spread">(30, 0, 180);
    auto const az = g.tile<stereo>(center - spread * 0.5, center + spread * 0.5);
    auto const fir = g.node<BaselineFir256>();

    auto const v_l = g.module<single_pan>();
    auto const v_r = g.module<single_pan>();

    auto const v_l_out = v_l(
        "in"_P = in[stereo::left],
        "azimuth"_P = az[stereo::left],
        "elevation"_P = el
    );

    auto const v_r_out = v_r(
        "in"_P = in[stereo::right],
        "azimuth"_P = az[stereo::right],
        "elevation"_P = el
    );

    fir(v_l_out + v_r_out);

    g.outputs("main"_P = fir);
}

IV_MODULE("iv.project.q24_bspline_pan", module_main);
