#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"
#include "graph_node.h"
#include "module_test_utils.h"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
    void detached_voice(iv::GraphBuilder& g, iv::SignalRef dt, iv::SignalRef noise, iv::Sample amplitude)
    {
        auto const reset = 1.0f;
        auto const frequency = 220.0f;
        auto const integrator = g.node<iv::Integrator>();
        auto const warper = g.node<iv::Warper>();

        integrator(warper["aliased"].detach() * reset, frequency * 2.0f, dt);
        warper(integrator + noise);
        g.outputs(warper["anti_aliased"] * amplitude);
    }
}

int main()
{
    iv::test::install_crash_handlers();

    iv::Sample dt_value = 1.0f / 48000.0f;
    iv::Sample noise_a = 0.125f;
    iv::Sample noise_b = -0.25f;
    std::vector<iv::Sample> output(32, 0.0f);

    iv::GraphBuilder graph;
    auto const dt = graph.node<iv::ValueSource>(&dt_value);
    auto const src_a = graph.node<iv::ValueSource>(&noise_a);
    auto const src_b = graph.node<iv::ValueSource>(&noise_b);
    auto const voice_a = graph.subgraph([&](iv::GraphBuilder& nested) {
        detached_voice(nested, nested.input("dt"), nested.input("noise"), 0.5f);
    });
    auto const voice_b = graph.subgraph([&](iv::GraphBuilder& nested) {
        detached_voice(nested, nested.input("dt"), nested.input("noise"), 0.25f);
    });
    auto const sink = graph.node<iv::BufferSink>(output.data(), output.size());

    voice_a({ {"dt", dt}, {"noise", src_a} });
    voice_b({ {"dt", dt}, {"noise", src_b} });
    sink(voice_a + voice_b);
    graph.outputs();

    iv::NodeProcessor processor(iv::TypeErasedNode(graph.build()), {}, output.size());
    processor.tick_block({}, 0, output.size());

    bool saw_non_zero = false;
    for (iv::Sample sample : output) {
        if (!std::isfinite(sample)) {
            std::cerr << "detach regression produced non-finite output\n";
            return 1;
        }
        if (sample != 0.0f) {
            saw_non_zero = true;
        }
    }

    iv::test::require(saw_non_zero, "detach regression should produce non-zero output");
    return 0;
}
