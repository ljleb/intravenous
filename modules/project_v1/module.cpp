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

inline void project_v1(iv::GraphBuilder& g)
{
    using namespace iv;
    auto make_channel = [&]<auto Channel>() {
        polyphonic<16>(g, [&]<size_t Voice>(auto m) {
            auto& [a, f] = m;
            auto saw = g.node<SawOscillator>();
            (void)Voice;
            g.outputs("main"_P[Channel] = saw("frequency"_P = f) * a);
        });
    };
    make_channel.template operator()<stereo::left>();
    make_channel.template operator()<stereo::right>();
}
