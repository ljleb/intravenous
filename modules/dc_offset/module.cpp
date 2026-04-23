#include "dsl.h"
#include "basic_nodes/noise.h"
#include "basic_nodes/filters.h"
#include "basic_nodes/shaping.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/midi.h"
#include "juce/vst_wrapper.h"

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
