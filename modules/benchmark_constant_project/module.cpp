#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>

void benchmark_constant_project(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const phase = g.node<PhaseIntegrator>();

    phase(0.0);
    auto const voice = g.node<"iv.test.benchmark_constant_project.voice">();
    auto const tone = voice(
        "amplitude"_P = 0.1,
        "frequency"_P = 110.0,
        "phase_offset"_P = phase
    );

    g.outputs("main"_P[stereo::left] = tone, "main"_P[stereo::right] = tone);
}

IV_MODULE("iv.test.benchmark_constant_project", benchmark_constant_project);
