#include "dsl.h"
#include "basic_nodes/shaping.h"

inline void reload_voice(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();

    auto const amplitude = g.input("amplitude", 0.1);
    auto const frequency = g.input("frequency", 220.0);
    auto const phase_offset = g.input("phase_offset", 0.0);
    auto const dt = g.input("dt", 1.0);

    auto const saw = g.node<SawOscillator>();
    saw(
        "frequency"_P = frequency,
        "phase_offset"_P = phase_offset,
        "dt"_P = dt
    );

    g.outputs("out"_P = saw * amplitude);
}

IV_EXPORT_MODULE("iv.test.reload_voice", reload_voice);
