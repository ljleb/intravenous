#include "module/module.h"
#include "basic_nodes/shaping.h"
#include "juce_vst_wrapper.h"

inline void noisy_saw_voice(iv::ModuleContext const& ctx)
{
    using namespace iv;
    auto& g = ctx.builder();
    auto const amp = g.input("amplitude", 0.1);
    auto const f = g.input("frequency", 500);
    auto const phi = g.input("phase_offset", 0);
    auto const dt = g.input("dt", 1);

    auto const saw = g.node<SawOscillator>();
    saw(
        "frequency"_P = f,
        "phase_offset"_P = phi,
        "dt"_P = dt
    );
    g.outputs("out"_P = saw * amp);
}

IV_EXPORT_MODULE("iv.test.noisy_saw_voice", noisy_saw_voice);
