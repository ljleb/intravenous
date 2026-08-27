#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>

consteval void behavior_voice(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const amplitude = g.input<"amplitude">(0.1);
    auto const frequency = g.input<"frequency">(220.0);
    auto const phase_offset = g.input<"phase_offset">(0.0);
    auto const saw = g.node<SawOscillator>();
    saw("frequency"_P = frequency, "phase_offset"_P = phase_offset);
    g.outputs("out"_P = saw * amplitude);
}
