#include <intravenous/dsl.h>

using iv::operator""_P;

void local_cmake_module(iv::GraphBuilder& g)
{
    g.outputs(
        "main"_P[iv::stereo::left] = 0.0f,
        "main"_P[iv::stereo::right] = 0.0f
    );
}
