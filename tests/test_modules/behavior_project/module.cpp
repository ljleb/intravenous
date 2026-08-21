#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>
#include <iv/modules/iv.test.behavior_voice>

inline void behavior_project(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const phase = g.node<PhaseIntegrator>();
    auto const voice = g.module<&behavior_voice>();
    phase(0.0);
    auto const tone = voice(
        "amplitude"_P = 0.25,
        "frequency"_P = 220.0,
        "phase_offset"_P = phase
    );
    g.outputs("main"_P[stereo::left] = tone, "main"_P[stereo::right] = tone);
}
