#include <intravenous/dsl.h>

void benchmark_constant_filtered(iv::GraphBuilder& g)
{
    using namespace iv;

    auto const phase = g.node<"phase_integrator">();
    auto const osc = g.node<"saw_oscillator">();
    auto const low_pass = g.node<"simple_iir_low_pass">();
    auto const high_pass = g.node<"simple_iir_high_pass">();

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

IV_MODULE("iv.test.benchmark_constant_filtered", benchmark_constant_filtered);
