#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/noise.h>
#include <intravenous/basic_nodes/filters.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/midi.h>
#include <intravenous/juce/vst_wrapper.h>

#include <array>
#include <iostream>
#include <string>

inline void noisy_saw_project(iv::ModuleContext const& c)
{
    using namespace iv;
    auto& g = c.builder();
    auto dt = g.node<ValueSource>(&c.sample_period());

    // auto const sup = juce::vst(g, "ValhallaSupermassive");
    // info(sup.node());
    // sup(
    //     "Mix"_P = 0.2,
    //     "DelayWarp"_P = 1.0,
    //     "Density"_P = 1.0,
    //     "Mode"_P = 2 / 24.f
    // );

    size_t seed = 0;
    SamplePortRef left;
    SamplePortRef right;
    g.multi_channel<ChannelTypeId::stereo>([&]<auto Ch>() {
        auto voice = polyphonic<16>(g, [&](auto m) {
            // m.connect_event_input("midi", midi);

            auto saw = g.node<SawOscillator>();
            auto phi = g.node<PhaseIntegrator>();
            auto generator = g.node<DeterministicUniformAESNoise>(seed++);
            auto u_to_n = g.node<UniformToGaussian>(0.0, 0.5);
            auto lo_pass = g.node<SimpleIirLowPass>();
            auto hi_pass = g.node<SimpleIirHighPass>();

            saw(
                "frequency"_P = m >> "frequency"_P,
                "phase_offset"_P = phi,
                "dt"_P = dt
            );
            phi < hi_pass * 0.1;
            hi_pass(lo_pass, 0.3, dt);
            lo_pass(u_to_n, 0.0, dt);
            u_to_n < generator;

            return saw * (m >> "amplitude"_P);
        });
        // voice < "midi"_F << juce::midi_input(g, "V25");

        auto lp = g.node<SimpleIirLowPass>();
        auto const channel_output = lp(voice, "dt"_P = dt) * 0.5;
        if constexpr (std::same_as<decltype(Ch), decltype(channels::stereo_left)>) {
            left = channel_output;
        } else {
            right = channel_output;
        }
    });

    g.outputs(channels::stereo_left = left, channels::stereo_right = right);
}

IV_EXPORT_MODULE("iv.test.noisy_saw", noisy_saw_project);
