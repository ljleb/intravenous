#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/filters.h>
#include <intravenous/basic_nodes/shaping.h>

constexpr void benchmark_constant_filtered(iv::GraphBuilder& g)
{
    using namespace iv;

    auto const phase = g.node<PhaseIntegrator>();
    auto const osc = g.node<SawOscillator>();
    auto const low_pass = g.node<SimpleIirLowPass>();
    auto const high_pass = g.node<SimpleIirHighPass>();

    phase(0.0);
    osc(
        "frequency"_P = 110.0,
        "phase_offset"_P = phase
    );
    low_pass(osc, 0.35);
    high_pass(low_pass, 0.08);

    auto const tone = high_pass * 0.12;
    g.outputs("main"_P[stereo::left] = tone, "main"_P[stereo::right] = tone);
}
