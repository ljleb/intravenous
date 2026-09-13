#include <intravenous/dsl.h>

void benchmark_constant_voice(iv::GraphBuilder& g)
{
    using namespace iv;

    auto const amplitude = g.input<"amplitude">(0.1);
    auto const frequency = g.input<"frequency">(110.0);
    auto const phase_offset = g.input<"phase_offset">(0.0);

    auto const saw = g.node<"saw_oscillator">();
    saw(
        "frequency"_P = frequency,
        "phase_offset"_P = phase_offset
    );

    g.outputs("out"_P = saw * amplitude);
}

IV_MODULE("iv.test.benchmark_constant_project.voice", benchmark_constant_voice);
