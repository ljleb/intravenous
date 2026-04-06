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

    auto const level_knob = 1.5;
    auto const lo_pass_knob = 1.0;
    auto const hi_pass_knob = 1.0;
    auto const generator = g.node<DeterministicUniformAESNoise>();
    auto const lo_pass = g.node<SimpleIirLowPass>();
    auto const hi_pass = g.node<SimpleIirHighPass>();

    auto const u_to_n_knob = 0.0;
    auto const u_to_n = g.node<UniformToGaussian>(0.0, 0.5);
    auto const u_to_c = g.node<UniformToCauchy>(0.0, 0.01);
    auto const interp = g.node<Interpolation>();

    generator("min"_P = -1, "max"_P = 1);

    u_to_c(generator);
    u_to_n(generator);

    interp(u_to_n, u_to_c, u_to_n_knob);

    lo_pass(interp, lo_pass_knob, dt);
    hi_pass(lo_pass, hi_pass_knob, dt);

    g.outputs(hi_pass * level_knob);
}

inline void noisy_saw_project(iv::ModuleContext const& context)
{
    using namespace iv;
    GraphBuilder& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = g.node<ValueSource>(&context.sample_period());
    SignalRef first_noise;

    auto const voice_builder = context.load_builder("iv.test.noisy_saw_voice");
    auto const midi = juce::midi_input(g, "V25") >> events;
    auto const sup = juce::vst(g, "D:\\music\\vst-plugins\\3\\x64\\ValhallaSupermassive.vst3");
    info(sup.node());

    for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
        auto const noise = g.subgraph(noise_voice);
        if (channel == 0) {
            first_noise = noise;
        }

        NodeRef shared_noise = g.node<Interpolation>();
        auto const sink = io.sink(g, channel);

        noise(dt);
        shared_noise = shared_noise(first_noise, noise, 1.0) * 0.1;
        auto const voice = polyphonic<16>(g, [&](NodeRef m) {
            m.connect_event_input("midi", midi);

            auto const voice = g.node(voice_builder);
            return voice(
                "noise"_P = shared_noise,
                "amplitude"_P = "amplitude"_P << m,
                "frequency"_P = "frequency"_P << m,
                "dt"_P = dt,
                "feedback"_P = ~("feedback"_P << voice)
            );
        });

        auto x = ("out"_P << voice);
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

IV_EXPORT_MODULE("iv.test.noisy_saw_project", noisy_saw_project);
