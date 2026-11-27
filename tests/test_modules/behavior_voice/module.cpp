#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>

void behavior_voice(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const amplitude = g.input({"Amplitude"}, {.default_value = 0.1});
    auto const frequency = g.input({"Frequency"}, {.default_value = 220.0});
    auto const phase_offset = g.input({"Phase offset"}, {.default_value = 0.0});
    auto const saw = g.node<"saw">();
    saw("frequency"_P = frequency, "phase_offset"_P = phase_offset);
    g.outputs("out"_P.with_display_name("Out") = saw * amplitude);
}

IV_MODULE("iv.test.behavior_voice", behavior_voice, "Behavior voice");
