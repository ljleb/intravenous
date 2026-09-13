#include <intravenous/dsl.h>
#include <intravenous/juce/vst_wrapper.h>

#include <array>
#include <iostream>
#include <string>

void dc_offset(iv::GraphBuilder& g)
{
    using namespace iv;
    g.outputs("main"_P[stereo::left] = 0.01, "main"_P[stereo::right] = 0.01);
}

IV_MODULE("iv.test.dc_offset", dc_offset);
