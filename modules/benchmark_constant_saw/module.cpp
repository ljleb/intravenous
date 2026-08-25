#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>

constexpr void benchmark_constant_saw(iv::GraphBuilder& g)
{
    using namespace iv;

    auto const phase = g.node<PhaseIntegrator>();
    auto const osc = g.node<SawOscillator>();
    phase(0.0);
    auto const tone = osc(
        "frequency"_P = 110.0,
        "phase_offset"_P = phase
    ) * 0.1;

    g.outputs("main"_P[stereo::left] = tone, "main"_P[stereo::right] = tone);
}
