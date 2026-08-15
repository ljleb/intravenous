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

inline void project_v1(iv::ModuleContext const& c)
{
    using namespace iv;
    auto& g = c.builder();
    g.multi_channel<stereo>([&]<auto Ch>() {
        polyphonic<16>(g, [&]<size_t Voice>(auto m) {
            auto& [a, f] = m;
            auto saw = g.node<SawOscillator>();
            (void)Voice;
            g.outputs("main"_P[Ch] = saw(f) * a);
        });
    });
}

IV_EXPORT_MODULE("iv.project.v1", project_v1);
