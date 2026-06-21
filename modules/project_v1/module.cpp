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
    SamplePortRef left;
    SamplePortRef right;

    g.multi_channel(ChannelTypeId::stereo, [&]<auto Ch>() {
        auto voice = polyphonic<16>(g, [&](auto m) {
            auto& [a, f] = m;
            auto saw = g.node<SawOscillator>();
            return saw(f) * a;
        });

        if constexpr (std::same_as<decltype(Ch), decltype(channels::stereo_left)>) {
            left = voice;
        } else {
            right = voice;
        }
    });

    g.outputs(channels::stereo_left = left, channels::stereo_right = right);
}

IV_EXPORT_MODULE("iv.project.v1", project_v1);
