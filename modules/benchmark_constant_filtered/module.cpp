#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/filters.h>
#include <intravenous/basic_nodes/shaping.h>

inline void benchmark_constant_filtered(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const dt = g.node<ValueSource>(&context.sample_period());

    auto const phase = g.node<PhaseIntegrator>();
    auto const osc = g.node<SawOscillator>();
    auto const low_pass = g.node<SimpleIirLowPass>();
    auto const high_pass = g.node<SimpleIirHighPass>();

    phase(0.0);
    osc(
        "frequency"_P = 110.0,
        "phase_offset"_P = phase,
        "dt"_P = dt
    );
    low_pass(osc, 0.35, dt);
    high_pass(low_pass, 0.08, dt);

    auto const tone = high_pass * 0.12;
    SamplePortRef left;
    SamplePortRef right;
    g.multi_channel<ChannelTypeId::stereo>([&]<auto Ch>() {
        if constexpr (std::same_as<decltype(Ch), decltype(channels::stereo_left)>) {
            left = tone;
        } else {
            right = tone;
        }
    });
    g.outputs(channels::stereo_left = left, channels::stereo_right = right);
}

IV_EXPORT_MODULE("iv.test.benchmark_constant_filtered", benchmark_constant_filtered);
