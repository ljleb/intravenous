#include "dsl.h"
#include "basic_nodes/noise.h"
#include "basic_nodes/filters.h"
#include "basic_nodes/shaping.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/midi.h"
#include "juce/vst_wrapper.h"

#include <array>
#include <iostream>
#include <string>

inline void noisy_saw_project(iv::ModuleContext const& c)
{
    using namespace iv;
    auto& g = c.builder();
    auto const& io = c.target_factory();
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
    for (size_t channel = 0; channel < c.render_config().num_channels; ++channel) {
        // auto const sink = io.file(g, channel, "out.wav");
        auto sink = io.sink(g, channel);

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
        sink(lp(voice, "dt"_P = dt) * 0.5);
    }

    g.outputs();
}

IV_EXPORT_MODULE("iv.test.noisy_saw", noisy_saw_project);
