#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/debug_probe.h>
#include <intravenous/basic_nodes/shaping.h>
#include <iv/nodes/iv.test.behavior_voice>

void behavior_project(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const phase = g.node<PhaseIntegrator>();
    auto const probe = g.node<DebugProbe>(DebugProbe{
        .label = "behavior probe",
        .every_n_ticks = 0,
    });
    auto const voice = g.node<"iv.test.behavior_voice">();
    phase(0.0);
    auto const tone = voice(
        "amplitude"_P = 0.25,
        "frequency"_P = 220.0,
        "phase_offset"_P = phase
    );
    probe(tone);
    g.outputs("main"_P[stereo::left] = probe, "main"_P[stereo::right] = probe);
}

IV_MODULE("iv.test.behavior_project", behavior_project);
