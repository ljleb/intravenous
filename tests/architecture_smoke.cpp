#include "devices/channel_buffer_sink.h"
#include "graph_node.h"
#include "module_test_utils.h"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
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
        iv::ModuleLoader loader(iv::test::repo_root());
        iv::NodeProcessor processor = iv::test::make_processor(
            loader,
            system,
            iv::test::test_modules_root() / "noisy_saw_project"
        );
        iv::test::require(processor.num_module_refs() != 0, "module refs should be retained by NodeProcessor");
        iv::test::run_processor_ticks(processor);
        iv::test::require(!system.is_shutdown_requested(), "directory module requested shutdown unexpectedly");
    }

    return 0;
}
