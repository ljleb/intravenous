#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/noise.h>
#include <intravenous/basic_nodes/filters.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/midi.h>
#include <intravenous/juce/vst_wrapper.h>

#include <array>
#include <iostream>
#include <string>

inline void noisy_saw_project(iv::ModuleContext const& context)
{
    using namespace iv;
    GraphBuilder& g = context.builder();
    g.outputs("main"_P[stereo::left] = 0.01, "main"_P[stereo::right] = 0.01);
}

IV_EXPORT_MODULE("iv.test.dc_offset", noisy_saw_project);
