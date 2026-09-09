#include <intravenous/dsl.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <string>

using namespace iv;

struct LearnedHrtfSource
{
    static constexpr size_t spatial_basis_size = 15;
    static constexpr size_t latent_parameter_count = 24;
    static constexpr size_t coefficient_count = spatial_basis_size * latent_parameter_count;

    static constexpr size_t pinna_filter_count = 8;

    static constexpr size_t input_history = 63;

    static constexpr float speed_of_sound = 343.0f;
    static constexpr float head_radius = 0.0875f;

    static constexpr char coefficient_path[] = "/home/ljleb/Downloads/P0014_v2_runtime_360param_coefficients.csv";

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

    struct Vec3
    {
        float x;
        float y;
        float z;
    };

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

        float delay_samples = 0;
    };

    struct State
    {
        std::array<float, coefficient_count> coefficients {};

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
        auto& state =
            ctx.state();

        load_coefficients(
            state.coefficients);

        // state.sample_rate = static_cast<float>(ctx.sample_rate);

        // if (state.sample_rate != 48000.0f) {
        //     throw std::runtime_error(
        //         "P0014 learned HRTF prototype requires 48 kHz");
        // }

        state.head_omega0 = speed_of_sound / head_radius;
        state.head_inverse_denominator = 1.0f / (state.head_omega0 + state.sample_rate);
        state.head_feedback = (state.sample_rate - state.head_omega0) * state.head_inverse_denominator;
        state.radius_over_c_samples = head_radius * state.sample_rate / speed_of_sound;
        update_direction(state, 0.0f, 0.0f);
    }

    void tick_block(TickBlockContext<LearnedHrtfSource> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.input<"in">();
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
                {
                    value = process_biquad( ear.filters[filter], ear.filter_state[filter], value);
                }

                if (channel == 0)
                {
                    output[stereo::left][i] = value;
                }
                else
                {
                    output[stereo::right][i] = value;
                }
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

    static void load_coefficients(std::array<float, coefficient_count>& coefficients)
    {
        std::ifstream file(coefficient_path);

        if (!file) {
            throw std::runtime_error(std::string("could not open ") + coefficient_path);
        }

        std::string line;

        // Header.
        std::getline(file, line);

        for (
            size_t row = 0;
            row < latent_parameter_count;
            ++row)
        {
            if (!std::getline(file, line))
                throw std::runtime_error("incomplete HRTF coefficient CSV");

            auto const first_comma = line.find(',');
            if (first_comma == std::string::npos)
                throw std::runtime_error("malformed HRTF coefficient CSV");

            char const* cursor = line.c_str() + first_comma + 1;

            for (size_t column = 0; column < spatial_basis_size; ++column)
            {
                char* end = nullptr;
                coefficients[row * spatial_basis_size + column] = std::strtof(cursor, &end);

                if (end == cursor)
                    throw std::runtime_error("malformed HRTF coefficient");

                cursor = end + (*end == ',');
            }
        }
    }

    static float softplus(float x)
    {
        return std::log1p(std::exp(x));
    }

    static float sigmoid(float x)
    {
        return 1.0f / (1.0f + std::exp(-x));
    }

    static float wendland_c2(float x, float y, float z, Vec3 const& center)
    {
        auto const dx = x - center.x;
        auto const dy = y - center.y;
        auto const dz = z - center.z;

        // All fitted support radii are pi/3.
        //
        // 1 - cos(pi/3) = 1/2, therefore
        //
        // r² = (1 - dot(s,c)) / (1/2)
        //    = 2(1 - dot(s,c))
        //    = |s-c|²
        //
        // for unit vectors s and c.
        auto const r2 = dx * dx + dy * dy + dz * dz;

        if (r2 >= 1.0f)
            return 0.0f;

        auto const r = std::sqrt(r2);
        auto const t = 1.0f - r;
        auto const t2 = t * t;
        auto const res = t2 * t2 * (4.0f * r + 1.0f);

        return res;
    }

    static std::array<float, spatial_basis_size>
    spatial_basis(float x, float y, float z)
    {
        constexpr auto inv_sqrt2 = 1.0f / std::numbers::sqrt2_v<float>;

        constexpr std::array<Vec3, 6> centers {{
            {
                +0.5f,
                -0.5f,
                -inv_sqrt2,
            },
            {
                +0.39713126196710286f,
                +0.8516507396391465f,
                -0.3420201433256687f,
            },
            {
                -0.6830127018922193f,
                +0.18301270189221952f,
                -inv_sqrt2,
            },
            {
                -0.10938165494661493f,
                -0.2345697160098045f,
                +0.9659258262890683f,
            },
            {
                -0.8830222215594891f,
                -0.3213938048432696f,
                -0.3420201433256687f,
            },
            {
                -0.25488700224417865f,
                -0.9512512425641977f,
                -0.17364817766693033f,
            },
        }};

        std::array<float, spatial_basis_size> result;

        result[0] = 1.0f;
        result[1] = x;
        result[2] = y;
        result[3] = z;

        result[4] = x * y;
        result[5] = x * z;
        result[6] = y * z;

        result[7] = x * x - y * y;

        result[8] = 3.0f * z * z - 1.0f;

        for (size_t i = 0; i < centers.size(); ++i) {
            result[9 + i] = wendland_c2(x, y, z, centers[i]);
        }

        return result;
    }

    static std::array<float, latent_parameter_count>
    evaluate_latent(State const& state, std::array<float, spatial_basis_size> const& basis)
    {
        std::array<float, latent_parameter_count> result;

        for (size_t row = 0; row < latent_parameter_count; ++row)
        {
            float value = 0.0f;

            auto const offset = row * spatial_basis_size;

            for (size_t column = 0; column < spatial_basis_size; ++column)
            {
                value += state.coefficients[offset + column] * basis[column];
            }

            result[row] = value;
        }

        return result;
    }

    static void decode_frequency_group(float const* latent, std::array<float, 4>& frequency)
    {
        float constexpr minimum_frequency = 1500.0f;
        float constexpr maximum_frequency = 22000.0f;

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

    static float process_head(
        EarRuntime& ear,
        float feedback,
        float input)
    {
        auto& state = ear.head_state;
        auto const output = ear.head.b0 * input + ear.head.b1 * state.previous_input + feedback * state.previous_output;

        state.previous_input = input;
        state.previous_output = output;

        return output;
    }

    static Sample read_fractional_delay(InputPort const& input, size_t frame, float delay)
    {
        auto const whole = static_cast<size_t>(delay);
        auto const fraction = delay - static_cast<float>(whole);
        auto const a = read_ago(input, frame, whole);
        auto const b = read_ago(input, frame, whole + 1);
        auto const res = a + fraction * (b - a);
        return res;
    }

    static void update_ear(State& state, size_t channel, float x, float y, float z)
    {
        auto const basis = spatial_basis(x, y, z);
        auto const latent = evaluate_latent(state, basis);
        auto const parameters = decode(latent);

        auto& ear = state.ear[channel];

        // Same provisional Brown-Duda head magnitude used during fitting.
        constexpr float alpha_min = 0.1f;
        constexpr float theta_min = 150.0f * std::numbers::pi_v<float> / 180.0f;
        auto const theta = std::acos(y);
        auto const head_theta = std::min(theta, theta_min);
        auto const alpha = (1.0f + alpha_min * 0.5f) + (1.0f - alpha_min * 0.5f) * std::cos(std::numbers::pi_v<float> * head_theta / theta_min);

        ear.head.b0 = (state.head_omega0 + alpha * state.sample_rate) * state.head_inverse_denominator;
        ear.head.b1 = (state.head_omega0 - alpha * state.sample_rate) * state.head_inverse_denominator;

        constexpr float half_pi = std::numbers::pi_v<float> / 2.0f;

        float relative_delay;
        if (theta < half_pi) {
            relative_delay = -state.radius_over_c_samples * y;
        }
        else {
            relative_delay = state.radius_over_c_samples * (theta - half_pi);
        }
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

    static void update_direction(State& state, float azimuth, float elevation)
    {
        auto const azimuth_radians = azimuth * std::numbers::pi_v<float> / 180.0f;
        auto const elevation_radians = elevation * std::numbers::pi_v<float> / 180.0f;
        auto const cos_elevation = std::cos(elevation_radians);
        auto const x = cos_elevation * std::cos(azimuth_radians);
        auto const y = -cos_elevation * std::sin(azimuth_radians);
        auto const z = std::sin(elevation_radians);

        update_ear(state, 0, x, y, z);
        update_ear(state, 1, x, -y, z);

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
                .channel_layout = {
                    .channel_type = ChannelTypeId::stereo,
                },
                .history = input_history,
            },
        };
    }

    static constexpr auto outputs()
    {
        return std::array<OutputConfig, 1> {
            OutputConfig {
                .name = "out",
                .channel_layout = {
                    .channel_type = ChannelTypeId::stereo,
                },
            },
        };
    }

    struct State
    {
        std::array<float, tap_count> taps {};
    };

    void initialize(
        InitializationContext<BaselineFir256> const& ctx) const
    {
        auto& state = ctx.state();

        std::ifstream file(coefficient_path);

        if (!file)
            throw std::runtime_error(
                std::string("could not open ")
                + coefficient_path);

        std::string line;

        // index,tap,value
        std::getline(file, line);

        for (size_t i = 0; i < tap_count; ++i)
        {
            if (!std::getline(file, line))
                throw std::runtime_error(
                    "incomplete baseline FIR CSV");

            auto const comma = line.rfind(',');

            if (comma == std::string::npos)
                throw std::runtime_error(
                    "malformed baseline FIR CSV");

            char* end = nullptr;

            state.taps[i] =
                std::strtof(
                    line.c_str() + comma + 1,
                    &end);

            if (end == line.c_str() + comma + 1)
                throw std::runtime_error(
                    "malformed baseline FIR coefficient");
        }
    }

    void tick_block(
        TickBlockContext<BaselineFir256> const& ctx) const
    {
        auto const& state = ctx.state();
        auto const& input = ctx.inputs[0];
        auto output = ctx.output<"out">();

        for (size_t frame = 0; frame < ctx.block_size; ++frame)
        {
            // Taps 0..frame are already in the current block.
            // Remaining taps live in InputPort history.
            auto const current_taps =
                std::min(frame + 1, tap_count);

            for (size_t channel = 0; channel < 2; ++channel)
            {
                float sum = 0.0f;

                for (
                    size_t tap = 0;
                    tap < current_taps;
                    ++tap)
                {
                    sum +=
                        state.taps[tap]
                        * input.get_frame(
                            frame - tap,
                            channel);
                }

                for (
                    size_t tap = current_taps;
                    tap < tap_count;
                    ++tap)
                {
                    sum +=
                        state.taps[tap]
                        * input.get(
                            tap - frame,
                            channel);
                }

                if (channel == 0)
                {
                    output[stereo::left][frame] = sum;
                }
                else
                {
                    output[stereo::right][frame] = sum;
                }
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
