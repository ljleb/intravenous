#include "module/module.h"
#include "basic_nodes/noise.h"
#include "basic_nodes/filters.h"
#include "basic_nodes/shaping.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/type_erased.h"
#include "juce_vst_wrapper.h"

#include <filesystem>

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

    generator({ {"min", -1}, {"max", 1} });

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
    auto const& targets = context.target_factory();
    auto voice_module = context.load("iv.test.noisy_saw_voice");

    auto const dt = g.node<ValueSource>(&context.sample_period());
    auto const zero = g.node<Constant>(Constant{0.0f});
    SignalRef first_noise;
    constexpr auto valhalla_path = "D:\\music\\vst-plugins\\3\\x64\\ValhallaSupermassive.vst3";
    auto const has_valhalla = std::filesystem::exists(valhalla_path);
    auto const valhalla = has_valhalla ? juce::vst(g, valhalla_path) : NodeRef();
    if (has_valhalla) {
        // info(valhalla.node());
        valhalla({
            {"Mix", 0.004},
            {"DelayWarp", 1.0},
            {"Delay_Ms", 0.0001},
            {"Density", 1.0},
            {"Feedback", 0.0},
            // {"Mode", 0.1},
        });
    }

    auto const val_idx = std::array {
        "l0",
        "r0",
    };

    for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
        auto const noise = g.subgraph(noise_voice);
        if (channel == 0) {
            first_noise = noise;
        }

        auto const voice = g.node(voice_module.builder());
        auto const shared_noise = g.node<Interpolation>();
        auto const sink = targets.sink(g, channel);
        SignalRef feedback = zero;

        noise(dt);
        shared_noise(first_noise, noise, 1.0);
        if (has_valhalla) {
            valhalla({{val_idx[channel], 1.0 * voice["feedback"].detach(1 << 9)}});
            feedback = valhalla[val_idx[channel]];
        }
        voice({
            {"noise", 0.0*shared_noise},
            {"dt", dt},
            {"feedback", feedback},
        });
        sink(voice["out"]);
    }

    g.outputs();
}

IV_EXPORT_MODULE("iv.test.noisy_saw_project", noisy_saw_project);
