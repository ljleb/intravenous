#include "module/module.h"
#include "basic_nodes/noise.h"
#include "basic_nodes/filters.h"
#include "basic_nodes/shaping.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/midi.h"
#include "basic_nodes/debug_probe.h"
#include "juce_vst_wrapper.h"

#include <array>
#include <iostream>
#include <string>

inline void noise_voice(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const dt = g.input("dt", 1.0);

    auto const generator = g.node<DeterministicUniformAESNoise>();
    auto const lo_pass = g.node<SimpleIirLowPass>();
    auto const hi_pass = g.node<SimpleIirHighPass>();
    auto const u_to_n = g.node<UniformToGaussian>(0.0, 0.5);

    u_to_n(generator);
    lo_pass(u_to_n, 0.0, dt);
    hi_pass(lo_pass, 0.3, dt);

    g.outputs(hi_pass * 0.1);
}

inline void noisy_saw_project(iv::ModuleContext const& context)
{
    using namespace iv;
    GraphBuilder& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = g.node<ValueSource>(&context.sample_period());

    auto const voice_builder = context.load_builder("iv.test.noisy_saw_voice");
    auto const midi = juce::midi_input(g) >> events;
    auto const sup = juce::vst(g, "D:\\music\\vst-plugins\\3\\x64\\ValhallaSupermassive.vst3");
    info(sup.node());
    sup(
        "DelayWarp"_P = 1.0,
        "Density"_P = 1.0,
        "Mode"_P = 2 / 24.f
    );

    for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
        // auto const sink = io.file(g, channel, "out.wav");
        auto const sink = io.sink(g, channel);

        auto const voice = polyphonic<16>(g, [&](NodeRef m) {
            static size_t seed = 0;
            m.connect_event_input("midi", midi);

            auto const saw = g.node<SawOscillator>();
            auto const amp = ("amplitude"_P << m);
            auto const f = ("frequency"_P << m);
            auto const phi = g.node<PhaseIntegrator>();
            auto const generator = g.node<DeterministicUniformAESNoise>(seed++);
            auto const u_to_n = g.node<UniformToGaussian>(0.0, 0.5);
            auto const lo_pass = g.node<SimpleIirLowPass>();
            auto const hi_pass = g.node<SimpleIirHighPass>();

            saw(
                "frequency"_P = f,
                "phase_offset"_P = phi,
                "dt"_P = dt
            );
            phi(hi_pass * 0.1);
            hi_pass(lo_pass, 0.3, dt);
            lo_pass(u_to_n, 0.0, dt);
            u_to_n(generator);

            return saw * amp;
        });

        SignalRef x = voice;
        if (channel == 0)
        {
            auto constexpr port = "l0"_P;
            x = port << sup(port = x);
        }
        else
        {
            auto constexpr port = "r0"_P;
            x = port << sup(port = x);
        }
        sink(x);
    }

    g.outputs();
}

IV_EXPORT_MODULE("iv.test.noisy_saw", noisy_saw_project);
