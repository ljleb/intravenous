#include "devices/channel_buffer_sink.h"
#include "graph_node.h"
#include "modules/module.h"
#include "modules/noisy_saw_project.h"
#include "runtime/system.h"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
    bool all_finite(std::vector<float> const& buffer)
    {
        for (float sample : buffer) {
            if (!std::isfinite(sample)) {
                return false;
            }
        }

        return true;
    }
}

int main()
{
    {
        iv::Sample a = 0.25f;
        iv::Sample b = 0.5f;
        std::array<iv::Sample, 8> channel {};
        iv::Sample* channels[] { channel.data() };
        iv::ChannelBufferTarget target;
        target.begin(channels, 1, channel.size(), 0);

        iv::GraphBuilder g;
        auto const src_a = g.node<iv::ValueSource>(&a);
        auto const src_b = g.node<iv::ValueSource>(&b);
        auto const sink_a = g.node<iv::ChannelBufferSink>(target, 0);
        auto const sink_b = g.node<iv::ChannelBufferSink>(target, 0);
        sink_a(src_a);
        sink_b(src_b);
        g.outputs();

        iv::NodeProcessor processor(iv::TypeErasedNode(std::move(g).build()));
        for (size_t i = 0; i < channel.size(); ++i) {
            processor.tick({}, i);
        }

        target.end();

        for (iv::Sample sample : channel) {
            if (sample != 0.75f) {
                std::cerr << "sink accumulation failed\n";
                return 1;
            }
        }
    }

    {
        iv::System system({}, false, false);
        iv::GraphBuilder g;
        iv::ModuleContext context(g, system);
        iv::TypeErasedModule module(iv::modules::noisy_saw_project);
        iv::TypeErasedNode root = system.wrap_root(module.build(context));
        iv::NodeProcessor processor(std::move(root));

        for (size_t i = 0; i < 16; ++i) {
            processor.tick({}, i);
        }

        if (system.is_shutdown_requested()) {
            std::cerr << "project module requested shutdown unexpectedly\n";
            return 1;
        }
    }

    return 0;
}
