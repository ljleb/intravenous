#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>

inline void benchmark_constant_saw(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const dt = g.node<ValueSource>(&context.sample_period());

    auto const phase = g.node<PhaseIntegrator>();
    auto const osc = g.node<SawOscillator>();
    phase(0.0);
    auto const tone = osc(
        "frequency"_P = 110.0,
        "phase_offset"_P = phase,
        "dt"_P = dt
    ) * 0.1;

    SamplePortRef left;
    SamplePortRef right;
    g.multi_channel(ChannelTypeId::stereo, [&]<auto Ch>() {
        if constexpr (std::same_as<decltype(Ch), decltype(channels::stereo_left)>) {
            left = tone;
        } else {
            right = tone;
        }
    });
    g.outputs(channels::stereo_left = left, channels::stereo_right = right);
}

IV_EXPORT_MODULE("iv.test.benchmark_constant_saw", benchmark_constant_saw);
