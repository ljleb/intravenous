#include <intravenous/dsl.h>

void benchmark_constant_voice(iv::GraphBuilder& g)
{
    using namespace iv;

    auto const amplitude = g.input(0.1);
    auto const frequency = g.input(110.0);
    auto const phase_offset = g.input(0.0);

    auto const saw = g.node<"saw">();
    saw(
        "frequency"_P = frequency,
        "phase_offset"_P = phase_offset
    );

    g.outputs("out"_P = saw * amplitude);
}

IV_MODULE("iv.test.benchmark_constant_voice", benchmark_constant_voice, "Benchmark Constant Voice");
