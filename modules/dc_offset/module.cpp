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
    SamplePortRef left;
    SamplePortRef right;
    g.multi_channel(ChannelTypeId::stereo, [&]<auto Ch>() {
        if constexpr (std::same_as<decltype(Ch), decltype(channels::stereo_left)>) {
            left = 0.01;
        } else {
            right = 0.01;
        }
    });
    g.outputs(channels::stereo_left = left, channels::stereo_right = right);
}

IV_EXPORT_MODULE("iv.test.dc_offset", noisy_saw_project);
