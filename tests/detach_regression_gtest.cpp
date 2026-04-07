#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"
#include "graph_node.h"
#include "module_test_utils.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

namespace {
    using namespace iv::literals;

    struct BufferSink {
        iv::Sample* destination;
        size_t size;

        auto inputs() const
        {
            return std::array<iv::InputConfig, 1>{};
        }

        void tick_block(iv::TickBlockContext<BufferSink> const& ctx) const
        {
            auto block = ctx.inputs[0].get_block(ctx.block_size);
            for (size_t i = 0; i < ctx.block_size; ++i) {
                size_t const index = ctx.index + i;
                if (index < size) {
                    destination[index] = block[i];
                }
            }
        }
    };

    void detached_voice(iv::GraphBuilder& g, iv::SignalRef dt, iv::SignalRef noise, iv::Sample amplitude)
    {
        auto const reset = 1.0f;
        auto const frequency = 220.0f;
        auto const integrator = g.node<iv::PhaseIntegrator>();
        auto const warper = g.node<iv::Warper>();

        integrator((warper["aliased"].detach() * reset + frequency * 2.0f) * dt);
        warper(integrator + noise);
        g.outputs(warper["anti_aliased"] * amplitude);
    }
}

TEST(DetachRegression, ProducesFiniteNonZeroOutput)
{
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
    auto const sink = graph.node<BufferSink>(output.data(), output.size());

    voice_a("dt"_P = dt, "noise"_P = src_a);
    voice_b("dt"_P = dt, "noise"_P = src_b);
    sink(voice_a + voice_b);
    graph.outputs();

    iv::test::FakeAudioDevice audio_device({ .max_block_frames = output.size() });
    iv::ExecutionTargetRegistry execution_targets(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(graph.build()),
        {},
        execution_targets,
        1
    );
    executor.tick_block(0, output.size());

    bool saw_non_zero = false;
    for (size_t i = 0; i < output.size(); ++i) {
        iv::Sample const sample = output[i];
        if (!std::isfinite(sample)) {
            FAIL() << "non-finite output at sample " << i << ": " << sample;
        }
        if (sample != 0.0f) {
            saw_non_zero = true;
        }
    }

    EXPECT_TRUE(saw_non_zero);
}

TEST(DetachRegression, NestedFeedbackWithoutDetachStillFailsAfterFlattening)
{
    iv::GraphBuilder graph;
    auto const recursive = graph.subgraph([](iv::GraphBuilder& nested) {
        auto const integrator = nested.node<iv::PhaseIntegrator>();
        integrator(integrator);
        nested.outputs(integrator);
    });
    (void)recursive;
    graph.outputs();

    try {
        graph.build();
    } catch (std::logic_error const& e) {
        EXPECT_NE(
            std::string_view(e.what()).find("graph contains a cycle"),
            std::string_view::npos
        );
        return;
    }

    FAIL() << "expected nested cycle without detach() to fail";
}
