#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>

void nested_loader_voice(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const amplitude = g.input(0.1);
    auto const frequency = g.input(220.0);
    auto const phase_offset = g.input(0.0);
    auto const saw = g.node<"saw">();
    saw("frequency"_P = frequency, "phase_offset"_P = phase_offset);
    g.outputs("out"_P = saw * amplitude);
}

IV_MODULE("iv.test.nested_loader_voice", nested_loader_voice, "Nested loader voice");
