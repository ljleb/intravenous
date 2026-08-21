#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>
#include <iv/modules/iv.test.reload_voice>

inline void reload_project(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const left_phase = g.node<PhaseIntegrator>();
    auto const right_phase = g.node<PhaseIntegrator>();
    auto const left_voice = g.module<&reload_voice>();
    auto const right_voice = g.module<&reload_voice>();
    left_phase(0.0);
    right_phase(0.0);
    g.outputs(
        "main"_P[stereo::left] = left_voice(
            "amplitude"_P = 0.25,
            "frequency"_P = 220.0,
            "phase_offset"_P = left_phase),
        "main"_P[stereo::right] = right_voice(
            "amplitude"_P = 0.25,
            "frequency"_P = 330.0,
            "phase_offset"_P = right_phase));
}
