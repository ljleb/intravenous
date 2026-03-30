#include "module/module.h"
#include "basic_nodes/shaping.h"
#include "juce_vst_wrapper.h"

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

inline void noisy_saw_voice(iv::ModuleContext const& ctx)
{
    using namespace iv;
    auto& g = ctx.builder();
    auto const amplitude = g.input("amplitude", 0.1);
    auto const frequency = g.input("frequency", 500.0);
    auto const voice_noise = g.input("noise");
    auto const dt = g.input("dt", 1.0);
    auto const feedback = g.input("feedback", 1.0);

    auto const integrator = g.node<Integrator>();
    auto const warper = g.node<Warper>();

    integrator(feedback, frequency * 2.0, dt);
    warper(integrator + voice_noise);
    g.outputs({
        {"out", warper["anti_aliased"] * amplitude},
        {"feedback", warper["aliased"]},
    });
}

IV_EXPORT_MODULE("iv.test.noisy_saw_voice", noisy_saw_voice);
