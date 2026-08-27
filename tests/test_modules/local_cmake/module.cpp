#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>

using namespace iv;

consteval void local_cmake_module(iv::GraphBuilder& g)
{
    auto const tone = g.node<iv::SawOscillator>();
    tone(
        "phase_offset"_P = 0.0,
        "frequency"_P = 440.0
    );
    g.outputs(
        "main"_P[iv::stereo::left] = tone,
        "main"_P[iv::stereo::right] = tone
    );
}
