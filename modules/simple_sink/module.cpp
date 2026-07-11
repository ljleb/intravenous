#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>

inline void simple_sink(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const dt = g.node<ValueSource>(&context.sample_period());
    SamplePortRef left;
    SamplePortRef right;

    g.multi_channel<ChannelTypeId::stereo>([&]<auto Ch>() {
        auto const phase = g.node<PhaseIntegrator>();
        auto const osc = g.node<SawOscillator>();
        constexpr Sample channel_offset =
            std::same_as<decltype(Ch), decltype(channels::stereo_left)> ? 0.0f : 1.0f;

        phase(0.0);
        auto const tone = osc(
            "frequency"_P = 110.0 + 55.0 * channel_offset,
            "phase_offset"_P = phase,
            "dt"_P = dt
        ) * 0.1;

        if constexpr (std::same_as<decltype(Ch), decltype(channels::stereo_left)>) {
            left = tone;
        } else {
            right = tone;
        }
    });

    g.outputs(channels::stereo_left = left, channels::stereo_right = right);
}

IV_EXPORT_MODULE("iv.test.simple_sink", simple_sink);
