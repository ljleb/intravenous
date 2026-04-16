#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"
#include "dsl.h"
#include "graph/node.h"
#include "module_test_utils.h"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
    using namespace iv;

    void tick_executor_direct(iv::NodeExecutor& executor, size_t index, size_t block_size)
    {
        iv::validate_block_size(block_size, "test block size must be a power of 2");
        if (block_size > executor.max_block_size()) {
            throw std::logic_error("test block size exceeds executor max block size");
        }

        executor.root().tick_block({
            iv::TickContext<iv::TypeErasedNode> {
                .inputs = {},
                .outputs = {},
                .event_inputs = {},
                .event_outputs = {},
                .buffer = executor.storage().buffer(),
            },
            index,
            block_size
        });
    }

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

    void detached_voice(iv::GraphBuilder& g, iv::SamplePortRef dt, iv::SamplePortRef noise, iv::Sample amplitude)
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
    auto const voice_a = graph.subgraph([&] {
        detached_voice(graph, dt, src_a, 0.5f);
    });
    auto const voice_b = graph.subgraph([&] {
        detached_voice(graph, dt, src_b, 0.25f);
    });
    auto const sink = graph.node<BufferSink>(output.data(), output.size());

    sink(voice_a + voice_b);
    graph.outputs();

    iv::test::FakeAudioDevice audio_device({ .max_block_frames = output.size() });
    iv::ExecutionTargetRegistry execution_targets(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(graph.build()),
        {},
        std::move(execution_targets).to_builder()
    );
    tick_executor_direct(executor, 0, output.size());

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
