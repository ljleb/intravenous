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

inline void project_v1(iv::ModuleContext const& c)
{
    using namespace iv;
    auto& g = c.builder();
    auto const& io = c.target_factory();

    for (size_t channel = 0; channel < c.render_config().num_channels; ++channel) {
        auto sink = io.sink(g, channel);

        auto voice = polyphonic<16>(g, [&](auto m) {
            auto& [a, f] = m;
            auto saw = g.node<SawOscillator>();
            return saw(f) * a;
        });

        sink(voice);
    }

    g.outputs();
}

IV_EXPORT_MODULE("iv.project.v1", project_v1);
