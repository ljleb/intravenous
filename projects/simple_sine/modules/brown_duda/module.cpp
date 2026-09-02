#include <intravenous/dsl.h>

using namespace iv;

constexpr ChannelLayout stereo_planar {
    .channel_type = ChannelTypeId::stereo,
    .sample_layout = SampleStreamLayout::planar,
};

constexpr float speed_of_sound = 343.0f;
constexpr float head_radius = 0.0875f;

constexpr float degrees_to_radians(float degrees)
{
    return degrees * std::numbers::pi_v<float> / 180.0f;
}

inline float wrap_degrees(float degrees)
{
    return std::remainder(degrees, 360.0f);
}

// Read "samples_ago" relative to frame `frame` in the current block.
//
// frame=10, samples_ago=3  -> current block frame 7
// frame=2,  samples_ago=5  -> 3 samples before current block
inline Sample read_ago(
    InputPort const& input,
    size_t frame,
    size_t samples_ago)
{
    if (samples_ago <= frame) {
        return input.get_frame(frame - samples_ago);
    }

    return input.get(samples_ago - frame);
}

template<size_t MaxDelay>
inline Sample read_fractional_delay(
    InputPort const& input,
    size_t frame,
    float delay)
{
    delay = std::clamp(
        delay,
        0.0f,
        static_cast<float>(MaxDelay - 1));

    auto const whole =
        static_cast<size_t>(std::floor(delay));

    auto const fraction =
        delay - static_cast<float>(whole);

    auto const a =
        read_ago(input, frame, whole);

    auto const b =
        read_ago(input, frame, whole + 1);

    return a + fraction * (b - a);
}

// -----------------------------------------------------------------------------
// 1. StereoSourceGeometry
//
// center_azimuth : interaural-polar azimuth, degrees
// width          : physical lateral source width, metres
// distance       : listener -> each source channel, metres, stereo
//
// output:
// azimuth         : actual azimuth of each source channel, stereo
//
// Both channels share elevation elsewhere.
//
// width/2 is the lateral displacement of each source from the center ray.
// If distance grows while width remains fixed, the angular stereo image
// naturally becomes narrower.
// -----------------------------------------------------------------------------

struct StereoSourceGeometry
{
    static constexpr auto inputs()
    {
        std::array<InputConfig, 3> result {};

        result[0].name = "center_azimuth";
        result[1].name = "width";

        result[2].name = "distance";
        result[2].channel_layout =
            brown_duda_detail::stereo_planar;

        return result;
    }

    static constexpr auto outputs()
    {
        std::array<OutputConfig, 1> result {};

        result[0].name = "azimuth";
        result[0].channel_layout =
            brown_duda_detail::stereo_planar;

        return result;
    }

    void tick_block(
        TickBlockContext<StereoSourceGeometry> const& ctx) const
    {
        auto const center =
            ctx.template input<"center_azimuth">();

        auto const width =
            ctx.template input<"width">();

        auto const distance =
            ctx.template input<"distance">();

        auto azimuth =
            ctx.template output<"azimuth">();

        constexpr float minimum_distance = 1.0e-4f;

        for (size_t i = 0; i < ctx.block_size; ++i) {
            auto const center_degrees =
                std::clamp(
                    static_cast<float>(center[i]),
                    -90.0f,
                    +90.0f);

            auto const half_width =
                0.5f * std::max(
                    0.0f,
                    static_cast<float>(width[i]));

            auto const left_distance =
                std::max(
                    minimum_distance,
                    static_cast<float>(
                        distance[stereo::left][i]));

            auto const right_distance =
                std::max(
                    minimum_distance,
                    static_cast<float>(
                        distance[stereo::right][i]));

            // If half_width > distance there is no triangle with that
            // listener->source range, so saturate at +/- 90 degrees.
            auto const left_ratio =
                std::clamp(
                    half_width / left_distance,
                    0.0f,
                    1.0f);

            auto const right_ratio =
                std::clamp(
                    half_width / right_distance,
                    0.0f,
                    1.0f);

            auto const left_offset =
                std::asin(left_ratio)
                * 180.0f
                / std::numbers::pi_v<float>;

            auto const right_offset =
                std::asin(right_ratio)
                * 180.0f
                / std::numbers::pi_v<float>;

            azimuth[stereo::left][i] =
                std::clamp(
                    center_degrees - left_offset,
                    -90.0f,
                    +90.0f);

            azimuth[stereo::right][i] =
                std::clamp(
                    center_degrees + right_offset,
                    -90.0f,
                    +90.0f);
        }
    }
};


// -----------------------------------------------------------------------------
// 2. BrownDudaParameters
//
// Does ALL angle -> ear parameter conversion.
//
// This is intentionally native stereo rather than tiled. It computes the
// geometry once and emits the paired left/right ear parameters.
//
// Inputs:
//   azimuth, elevation
//
// Outputs:
//   head_b0          stereo
//   head_b1          stereo
//   head_feedback    mono, same for both ears
//   head_delay       stereo, in samples
//   d2 ... d6        stereo, pinna delays in samples
//
// The head filter coefficients are the bilinear-transform realization of
// Brown & Duda's analog one-pole / one-zero filter. The paper specifies the
// transfer function but not the exact analog->digital transform.
// -----------------------------------------------------------------------------

enum class BrownDudaPinnaProfile {
    PB_NH,
    RD,
};

struct BrownDudaParameters
{
    BrownDudaPinnaProfile pinna_profile =
        BrownDudaPinnaProfile::PB_NH;

    constexpr BrownDudaParameters() = default;

    constexpr explicit BrownDudaParameters(
        BrownDudaPinnaProfile profile)
        : pinna_profile(profile)
    {}

    static constexpr auto inputs()
    {
        std::array<InputConfig, 2> result {};

        result[0].name = "azimuth";
        result[1].name = "elevation";

        return result;
    }

    static constexpr auto outputs()
    {
        std::array<OutputConfig, 9> result {};

        result[0].name = "head_b0";
        result[0].channel_layout =
            brown_duda_detail::stereo_planar;

        result[1].name = "head_b1";
        result[1].channel_layout =
            brown_duda_detail::stereo_planar;

        result[2].name = "head_feedback";
        // mono: broadcasts into the tiled head filter

        result[3].name = "head_delay";
        result[3].channel_layout =
            brown_duda_detail::stereo_planar;

        result[4].name = "d2";
        result[4].channel_layout =
            brown_duda_detail::stereo_planar;

        result[5].name = "d3";
        result[5].channel_layout =
            brown_duda_detail::stereo_planar;

        result[6].name = "d4";
        result[6].channel_layout =
            brown_duda_detail::stereo_planar;

        result[7].name = "d5";
        result[7].channel_layout =
            brown_duda_detail::stereo_planar;

        result[8].name = "d6";
        result[8].channel_layout =
            brown_duda_detail::stereo_planar;

        return result;
    }

private:
    static float alpha(float incidence_degrees)
    {
        constexpr float alpha_min = 0.1f;
        constexpr float theta_min = 150.0f;

        return
            (1.0f + alpha_min * 0.5f)
            +
            (1.0f - alpha_min * 0.5f)
            * std::cos(
                std::numbers::pi_v<float>
                * incidence_degrees
                / theta_min);
    }

    static float propagation_delay_seconds(
        float incidence_degrees)
    {
        using namespace brown_duda_detail;

        auto const theta =
            degrees_to_radians(
                wrap_degrees(incidence_degrees));

        auto const absolute_theta =
            std::abs(theta);

        auto const radius_over_c =
            head_radius / speed_of_sound;

        float relative_delay = 0.0f;

        if (absolute_theta < std::numbers::pi_v<float> * 0.5f) {
            relative_delay =
                -radius_over_c * std::cos(theta);
        } else {
            relative_delay =
                radius_over_c
                * (
                    absolute_theta
                    - std::numbers::pi_v<float> * 0.5f
                );
        }

        // Brown & Duda add a/c so every delay is causal.
        return radius_over_c + relative_delay;
    }

    float pinna_d(size_t event) const
    {
        if (pinna_profile == BrownDudaPinnaProfile::RD) {
            return event == 0 ? 0.85f : 0.35f;
        }

        // PB and NH
        return event == 0 ? 1.0f : 0.5f;
    }

    float pinna_delay_44100(
        size_t event,
        float azimuth,
        float elevation) const
    {
        // Table I, rows n=2..6.
        constexpr std::array<float, 5> A {
            1.0f,
            5.0f,
            5.0f,
            5.0f,
            5.0f,
        };

        constexpr std::array<float, 5> B {
            2.0f,
            4.0f,
            7.0f,
            11.0f,
            13.0f,
        };

        auto const azimuth_factor =
            std::cos(
                brown_duda_detail::degrees_to_radians(
                    azimuth * 0.5f));

        auto const elevation_factor =
            std::sin(
                brown_duda_detail::degrees_to_radians(
                    pinna_d(event)
                    * (90.0f - elevation)));

        return
            A[event]
            * azimuth_factor
            * elevation_factor
            + B[event];
    }

public:
    void tick_block(
        TickBlockContext<BrownDudaParameters> const& ctx) const
    {
        using namespace brown_duda_detail;

        auto const azimuth =
            ctx.template input<"azimuth">();

        auto const elevation =
            ctx.template input<"elevation">();

        auto head_b0 =
            ctx.template output<"head_b0">();

        auto head_b1 =
            ctx.template output<"head_b1">();

        auto head_feedback =
            ctx.template output<"head_feedback">();

        auto head_delay =
            ctx.template output<"head_delay">();

        auto d2 = ctx.template output<"d2">();
        auto d3 = ctx.template output<"d3">();
        auto d4 = ctx.template output<"d4">();
        auto d5 = ctx.template output<"d5">();
        auto d6 = ctx.template output<"d6">();

        auto const sample_rate =
            static_cast<float>(ctx.sample_rate);

        // Brown-Duda omega_0 = c / a.
        auto const omega0 =
            speed_of_sound / head_radius;

        // Bilinear transform.
        auto const denominator =
            omega0 + sample_rate;

        // This coefficient is identical for both ears and independent
        // of direction, so calculate it once for the block.
        auto const feedback =
            (sample_rate - omega0)
            / denominator;

        auto const pinna_sample_rate_scale =
            sample_rate / 44100.0f;

        for (size_t i = 0; i < ctx.block_size; ++i) {
            auto const source_azimuth =
                std::clamp(
                    static_cast<float>(azimuth[i]),
                    -90.0f,
                    +90.0f);

            auto const source_elevation =
                std::clamp(
                    static_cast<float>(elevation[i]),
                    -90.0f,
                    +90.0f);

            // Brown & Duda use approximately:
            //
            // right ear entrance: +100 degrees
            // left  ear entrance: -100 degrees

            auto const left_incidence =
                wrap_degrees(
                    source_azimuth - (-100.0f));

            auto const right_incidence =
                wrap_degrees(
                    source_azimuth - (+100.0f));

            auto const alpha_left =
                alpha(left_incidence);

            auto const alpha_right =
                alpha(right_incidence);

            head_b0[stereo::left][i] =
                (omega0 + alpha_left * sample_rate)
                / denominator;

            head_b1[stereo::left][i] =
                (omega0 - alpha_left * sample_rate)
                / denominator;

            head_b0[stereo::right][i] =
                (omega0 + alpha_right * sample_rate)
                / denominator;

            head_b1[stereo::right][i] =
                (omega0 - alpha_right * sample_rate)
                / denominator;

            head_feedback[i] =
                feedback;

            head_delay[stereo::left][i] =
                propagation_delay_seconds(left_incidence)
                * sample_rate;

            head_delay[stereo::right][i] =
                propagation_delay_seconds(right_incidence)
                * sample_rate;

            auto const delay2 =
                pinna_delay_44100(
                    0,
                    source_azimuth,
                    source_elevation)
                * pinna_sample_rate_scale;

            auto const delay3 =
                pinna_delay_44100(
                    1,
                    source_azimuth,
                    source_elevation)
                * pinna_sample_rate_scale;

            auto const delay4 =
                pinna_delay_44100(
                    2,
                    source_azimuth,
                    source_elevation)
                * pinna_sample_rate_scale;

            auto const delay5 =
                pinna_delay_44100(
                    3,
                    source_azimuth,
                    source_elevation)
                * pinna_sample_rate_scale;

            auto const delay6 =
                pinna_delay_44100(
                    4,
                    source_azimuth,
                    source_elevation)
                * pinna_sample_rate_scale;

            // Eq. 8 is even in azimuth because of cos(theta / 2),
            // so the published generic model has the same event times
            // in the symmetric left and right pinnae.

            d2[stereo::left][i] = delay2;
            d2[stereo::right][i] = delay2;

            d3[stereo::left][i] = delay3;
            d3[stereo::right][i] = delay3;

            d4[stereo::left][i] = delay4;
            d4[stereo::right][i] = delay4;

            d5[stereo::left][i] = delay5;
            d5[stereo::right][i] = delay5;

            d6[stereo::left][i] = delay6;
            d6[stereo::right][i] = delay6;
        }
    }
};


// -----------------------------------------------------------------------------
// 3. OnePoleOneZero
//
// Generic time-domain DSP node.
//
// y[n] = b0*x[n] + b1*x[n-1] + feedback*y[n-1]
//
// It has absolutely no concept of ears, azimuth, head radius, etc.
// g.node<OnePoleOneZero, stereo>() supplies two independent filter states.
// -----------------------------------------------------------------------------

struct OnePoleOneZero
{
    static constexpr auto inputs()
    {
        std::array<InputConfig, 4> result {};

        result[0].name = "in";
        result[0].history = 1;

        result[1].name = "b0";
        result[2].name = "b1";
        result[3].name = "feedback";

        return result;
    }

    static constexpr auto outputs()
    {
        std::array<OutputConfig, 1> result {};
        result[0].name = "out";
        return result;
    }

    void tick_block(
        TickBlockContext<OnePoleOneZero> const& ctx) const
    {
        auto const input =
            ctx.template input<"in">();

        auto const b0 =
            ctx.template input<"b0">();

        auto const b1 =
            ctx.template input<"b1">();

        auto const feedback =
            ctx.template input<"feedback">();

        auto output =
            ctx.template output<"out">();

        auto previous_input =
            ctx.inputs[0].get(1);

        auto previous_output =
            ctx.outputs[0].get();

        for (size_t i = 0; i < ctx.block_size; ++i) {
            auto const current_input =
                input[i];

            auto const current_output =
                  b0[i] * current_input
                + b1[i] * previous_input
                + feedback[i] * previous_output;

            output[i] = current_output;

            previous_input = current_input;
            previous_output = current_output;
        }
    }
};


// -----------------------------------------------------------------------------
// 4. SampleDelay
//
// Pure time delay.
//
// Brown & Duda's complexity description treats the propagation delay as
// memory with no weighting, so this baseline rounds to the nearest sample
// rather than interpolating.
//
// A fractional-delay replacement can be added later without touching any
// Brown-Duda geometry.
// -----------------------------------------------------------------------------

struct SampleDelay
{
    static constexpr size_t max_delay_samples = 1024;

    static constexpr auto inputs()
    {
        std::array<InputConfig, 2> result {};

        result[0].name = "in";
        result[0].history = max_delay_samples;

        result[1].name = "delay_samples";

        return result;
    }

    static constexpr auto outputs()
    {
        std::array<OutputConfig, 1> result {};
        result[0].name = "out";
        return result;
    }

    void tick_block(
        TickBlockContext<SampleDelay> const& ctx) const
    {
        auto const delays =
            ctx.template input<"delay_samples">();

        auto output =
            ctx.template output<"out">();

        auto const& input =
            ctx.inputs[0];

        for (size_t i = 0; i < ctx.block_size; ++i) {
            auto const delay =
                std::clamp(
                    static_cast<float>(delays[i]),
                    0.0f,
                    static_cast<float>(
                        max_delay_samples - 1));

            auto const integer_delay =
                static_cast<size_t>(
                    std::lround(delay));

            output[i] =
                brown_duda_detail::read_ago(
                    input,
                    i,
                    integer_delay);
        }
    }
};


// -----------------------------------------------------------------------------
// 5. BrownDudaPinna
//
// Published generic pinna model:
//
// direct event
// + .50  * delayed event 2
// - 1.00 * delayed event 3
// + .50  * delayed event 4
// - .25  * delayed event 5
// + .25  * delayed event 6
//
// Brown & Duda explicitly linearly split fractional pinna event positions
// between the surrounding samples. `read_fractional_delay` is exactly the
// equivalent time-domain operation.
// -----------------------------------------------------------------------------

struct BrownDudaPinna
{
    static constexpr size_t max_delay_samples = 1024;

    static constexpr auto inputs()
    {
        std::array<InputConfig, 6> result {};

        result[0].name = "in";
        result[0].history = max_delay_samples;

        result[1].name = "d2";
        result[2].name = "d3";
        result[3].name = "d4";
        result[4].name = "d5";
        result[5].name = "d6";

        return result;
    }

    static constexpr auto outputs()
    {
        std::array<OutputConfig, 1> result {};
        result[0].name = "out";
        return result;
    }

    void tick_block(
        TickBlockContext<BrownDudaPinna> const& ctx) const
    {
        auto const d2 =
            ctx.template input<"d2">();

        auto const d3 =
            ctx.template input<"d3">();

        auto const d4 =
            ctx.template input<"d4">();

        auto const d5 =
            ctx.template input<"d5">();

        auto const d6 =
            ctx.template input<"d6">();

        auto output =
            ctx.template output<"out">();

        auto const& input =
            ctx.inputs[0];

        for (size_t i = 0; i < ctx.block_size; ++i) {
            Sample y =
                input.get_frame(i);

            y += 0.50f
                * brown_duda_detail::read_fractional_delay<
                    max_delay_samples>(
                        input, i, d2[i]);

            y -= 1.00f
                * brown_duda_detail::read_fractional_delay<
                    max_delay_samples>(
                        input, i, d3[i]);

            y += 0.50f
                * brown_duda_detail::read_fractional_delay<
                    max_delay_samples>(
                        input, i, d4[i]);

            y -= 0.25f
                * brown_duda_detail::read_fractional_delay<
                    max_delay_samples>(
                        input, i, d5[i]);

            y += 0.25f
                * brown_duda_detail::read_fractional_delay<
                    max_delay_samples>(
                        input, i, d6[i]);

            output[i] = y;
        }
    }
};

consteval void brown_duda_source(iv::GraphBuilder& g)
{
    auto const source = g.input<"source">();
    auto const azimuth = g.input<"azimuth">(0.0f);
    auto const elevation = g.input<"elevation">(0.0f);

    auto const params = g.node<BrownDudaParameters>();
    auto const shadow = g.node<HeadShadow, stereo>();
    auto const propagation = g.node<VariableDelay, stereo>();
    auto const pinna = g.node<Pinna, stereo>();

    params(
        "azimuth"_P = azimuth,
        "elevation"_P = elevation);
    shadow(
        "in"_P = source,
        "alpha"_P = params["alpha"]);
    propagation(
        "in"_P = shadow,
        "delay"_P = params["delay"]);
    pinna(
        "in"_P = propagation,
        "d2"_P = params["d2"],
        "d3"_P = params["d3"],
        "d4"_P = params["d4"],
        "d5"_P = params["d5"],
        "d6"_P = params["d6"]
    );

    g.outputs(
        "main"_P = pinna
    );
}

consteval void module_main(iv::GraphBuilder& g)
{
    auto const audio = g.input<"main", stereo>();
    auto const center = g.input<"azimuth">(0);
    auto const spread = g.input<"spread">(30, 0, 180);
    auto const elevation = g.input<"elevation">(0, -90, 90);
    auto const source_left = g.module<brown_duda_source>();
    auto const source_right = g.module<brown_duda_source>();

    auto const azimuths = g.tile<stereo>(center - spread * 0.5, center + spread * 0.5);

    auto const spatialized_left = source_left(
        "source"_P = audio[stereo::left],
        "azimuth"_P = azimuths[stereo::left],
        "elevation"_P = elevation
    );

    auto const spatialized_right = source_right(
        "source"_P = audio[stereo::right],
        "azimuth"_P = azimuths[stereo::right],
        "elevation"_P = elevation
    );

    g.outputs("main"_P = spatialized_left + spatialized_right);
}
