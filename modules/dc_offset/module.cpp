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
    auto const& io = context.target_factory();

    for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
        // auto const sink = io.file(g, channel, "out.wav");
        auto const sink = io.sink(g, channel);
        sink(0.01);
    }

    g.outputs();
}

IV_EXPORT_MODULE("iv.test.dc_offset", noisy_saw_project);
