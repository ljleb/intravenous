#include "module/module.h"
#include "basic_nodes/noise.h"
#include "basic_nodes/filters.h"
#include "basic_nodes/shaping.h"
#include "basic_nodes/buffers.h"
#include "juce_vst_wrapper.h"

#include <iostream>
#include <string>

// struct DebugProbe {
//     std::string label;

//     auto inputs() const
//     {
//         return std::array {
//             iv::InputConfig { "in" },
//         };
//     }

//     auto outputs() const
//     {
//         return std::array {
//             iv::OutputConfig { "out" },
//         };
//     }

//     void tick_block(iv::TickBlockContext<DebugProbe> const& ctx) const
//     {
//         auto input = ctx.inputs[0].get_block(ctx.block_size);
//         ctx.outputs[0].push_block(input);

//         iv::Sample max_abs = 0;
//         for (iv::Sample sample : input) {
//             max_abs = std::max(max_abs, static_cast<iv::Sample>(std::abs(sample)));
//         }
//         if (max_abs == 0) {
//             std::cerr
//                 << "DebugProbe silence: label=" << label
//                 << " index=" << ctx.index
//                 << " size=" << ctx.block_size
//                 << '\n';
//         }
//     }
// };

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
    SignalRef first_noise;
    // auto const valhalla = juce::vst(g, "D:\\music\\vst-plugins\\3\\x64\\ValhallaSupermassive.vst3");
    // valhalla({
    //     {"Mix", 0.004},
    //     {"DelayWarp", 1.0},
    //     {"Delay_Ms", 0.0001},
    //     {"Density", 1.0},
    //     {"Feedback", 0.0},
    //     // {"Mode", 0.1},
    // });

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

        noise(dt);
        shared_noise(first_noise, noise, 1.0);
        // valhalla({{val_idx[channel], 1.0 * voice["feedback"].detach(4)}});
        voice({
            {"noise", 0.1*shared_noise},
            {"dt", dt},
            {"feedback", voice["feedback"].detach()},
            // {"feedback", valhalla[val_idx[channel]]},
        });
        sink(voice["out"]);
    }

    g.outputs();
}

IV_EXPORT_MODULE("iv.test.noisy_saw_project", noisy_saw_project);
