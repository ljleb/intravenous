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

consteval void noisy_saw_project(iv::GraphBuilder& g)
{
    using namespace iv;

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
    auto make_channel = [&]<auto Ch>() {
        auto const saw = g.node<SawOscillator>();
        auto const phi = g.node<PhaseIntegrator>();
        auto const generator = g.node<DeterministicUniformAESNoise>(seed++);
        auto const u_to_n = g.node<UniformToGaussian>(0.0, 0.5);
        auto const lo_pass = g.node<SimpleIirLowPass>();
        auto const hi_pass = g.node<SimpleIirHighPass>();
        auto const lp = g.node<SimpleIirLowPass>();

        u_to_n(generator);
        lo_pass(u_to_n, 0.0);
        hi_pass(lo_pass, 0.3);
        phi(hi_pass * 0.1);
        saw(
            "frequency"_P = 110.0,
            "phase_offset"_P = phi
        );
        lp(saw * 0.1, 0.5);
        auto const channel_output = lp * 0.5;
        if constexpr (Ch == stereo::left) {
            left = channel_output;
        } else {
            right = channel_output;
        }
    };
    make_channel.template operator()<stereo::left>();
    make_channel.template operator()<stereo::right>();

    g.outputs("main"_P = g.tile<stereo>(left, right));
}
