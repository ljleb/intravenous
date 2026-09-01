#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/dsl.h>
#include <intravenous/node/block_executor.h>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace {
    using namespace iv;

    enum class RuntimeValueSlot : size_t {
        dt,
        noise_a,
        noise_b,
    };

    std::array<iv::Sample, 3> runtime_values {};
    std::span<iv::Sample> runtime_output {};

    struct RuntimeValueSource {
        RuntimeValueSlot slot {};

        static constexpr auto outputs()
        {
            return std::array { iv::OutputConfig { .name = "value" } };
        }

        void tick(iv::TickSampleContext<RuntimeValueSource> const& ctx) const
        {
            ctx.outputs[0].push(runtime_values[static_cast<size_t>(slot)]);
        }
    };

    struct RuntimeBufferSink {
        static constexpr auto inputs()
        {
            return std::array<iv::InputConfig, 1>{};
        }

        void tick_block(iv::TickBlockContext<RuntimeBufferSink> const& ctx) const
        {
            auto const block = ctx.inputs[0].get_block(ctx.block_size);
            for (size_t i = 0; i < ctx.block_size; ++i) {
                auto const index = ctx.index + i;
                if (index < runtime_output.size()) {
                    runtime_output[index] = block[i];
                }
            }
        }
    };

    consteval void detached_voice(
        iv::GraphBuilder& g,
        iv::SubgraphBuilder& boundary,
        iv::SamplePortRef dt,
        iv::SamplePortRef noise,
        iv::Sample amplitude)
    {
        auto const reset = 1.0f;
        auto const frequency = 220.0f;
        auto const integrator = g.node<iv::PhaseIntegrator>();
        auto const warper = g.node<iv::Warper>();

        integrator((warper["aliased"].detach() * reset + frequency * 2.0f) * dt);
        warper(integrator + noise);
        boundary.outputs("out"_P = (warper["anti_aliased"] * amplitude));
    }

    consteval auto build_detached_graph()
    {
        iv::GraphBuilder graph;
        auto const dt = graph.node<RuntimeValueSource>(RuntimeValueSlot::dt);
        auto const src_a = graph.node<RuntimeValueSource>(RuntimeValueSlot::noise_a);
        auto const src_b = graph.node<RuntimeValueSource>(RuntimeValueSlot::noise_b);
        auto const voice_a = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
            detached_voice(graph, boundary, dt, src_a, 0.5f);
        });
        auto const voice_b = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
            detached_voice(graph, boundary, dt, src_b, 0.25f);
        });
        auto const sink = graph.node<RuntimeBufferSink>();

        sink(voice_a + voice_b);
        graph.outputs();
        return std::move(graph).build().graph;
    }

    static constexpr auto detached_graph = build_detached_graph();
    static constexpr iv::StaticGraphRoot<detached_graph> static_detached_graph {};
    static constexpr iv::RuntimeGraphRoot runtime_detached_graph {detached_graph};

    consteval auto build_static_dormancy_graph()
    {
        iv::GraphBuilder graph;
        auto const source = graph.node<iv::Constant>(iv::Sample{0.0f});
        auto const nested = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
            auto const input = boundary.input<"in">(0.0f);
            auto const pass = graph.node<iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>>();
            pass(input);
            boundary.outputs("out"_P = pass);
        }).ttl(1);
        nested("in"_P = source);
        graph.outputs("out"_P = nested);
        return std::move(graph).build({.execution_root = true}).graph;
    }

    static constexpr auto static_dormancy_graph = build_static_dormancy_graph();
    static_assert(static_dormancy_graph._dormancy_groups.size != 0);
    static constexpr iv::StaticGraphRoot<static_dormancy_graph>
        static_dormancy_root {};
    static constexpr iv::RuntimeGraphRoot runtime_dormancy_root {
        static_dormancy_graph};

    // An unconnected subgraph input lowers through materialize_subgraph_default.
    // Its default must reach a block-reading node without leaving a ticking
    // Constant wrapper in the executable graph.
    consteval auto build_static_default_sink_graph()
    {
        iv::GraphBuilder graph;
        auto const nested = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
            auto const input = boundary.input<"in">(iv::Sample{0.375f});
            auto const pass = graph.node<
                iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>>();
            pass(input);
            boundary.outputs("out"_P = pass);
        });
        auto const sink = graph.node<RuntimeBufferSink>();
        sink(nested);
        graph.outputs();
        return std::move(graph).build().graph;
    }

    static constexpr auto static_default_sink_graph =
        build_static_default_sink_graph();
    static constexpr iv::StaticGraphRoot<static_default_sink_graph>
        static_default_sink_root {};
    static constexpr iv::RuntimeGraphRoot runtime_default_sink_root {
        static_default_sink_graph};

    void tick_executor_direct(
        iv::BlockNodeExecutor& executor,
        size_t index,
        size_t block_size)
    {
        iv::validate_block_size(
            block_size, "test block size must be a power of 2");
        if (block_size != executor.block_size()) {
            throw std::logic_error(
                "test block size must match block executor block size");
        }
        executor.tick_block(index);
    }
}

TEST(DetachRegression, ProducesFiniteNonZeroOutput)
{
    runtime_values[static_cast<size_t>(RuntimeValueSlot::dt)] =
        iv::Sample{1.0f / 48000.0f};
    runtime_values[static_cast<size_t>(RuntimeValueSlot::noise_a)] =
        iv::Sample{0.125f};
    runtime_values[static_cast<size_t>(RuntimeValueSlot::noise_b)] =
        iv::Sample{-0.25f};

    std::vector<iv::Sample> output(32, 0.0f);
    runtime_output = output;

    iv::BlockNodeExecutor executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(static_detached_graph),
        output.size());
    tick_executor_direct(executor, 0, output.size());
    runtime_output = {};

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

TEST(DetachRegression, RuntimeGraphRootMatchesStaticGraphRootOutput)
{
    runtime_values[static_cast<size_t>(RuntimeValueSlot::dt)] =
        iv::Sample{1.0f / 48000.0f};
    runtime_values[static_cast<size_t>(RuntimeValueSlot::noise_a)] =
        iv::Sample{0.125f};
    runtime_values[static_cast<size_t>(RuntimeValueSlot::noise_b)] =
        iv::Sample{-0.25f};

    std::vector<iv::Sample> static_output(32, 0.0f);
    runtime_output = static_output;
    auto static_executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(static_detached_graph), static_output.size());
    tick_executor_direct(static_executor, 0, static_output.size());

    std::vector<iv::Sample> runtime_root_output(32, 0.0f);
    runtime_output = runtime_root_output;
    auto runtime_executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(runtime_detached_graph), runtime_root_output.size());
    tick_executor_direct(runtime_executor, 0, runtime_root_output.size());
    runtime_output = {};

    ASSERT_EQ(runtime_root_output.size(), static_output.size());
    for (size_t i = 0; i < static_output.size(); ++i) {
        EXPECT_EQ(runtime_root_output[i], static_output[i])
            << "output differs at sample " << i;
    }
}

TEST(DetachRegression, StaticGraphRootExecutesDormancyGroups)
{
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(static_dormancy_root), 8);

    executor.tick_block(0);
    executor.tick_block(8);
}

TEST(DetachRegression, RuntimeGraphRootExecutesDormancyGroups)
{
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(runtime_dormancy_root), 8);

    executor.tick_block(0);
    executor.tick_block(8);
}

TEST(DetachRegression, StaticSubgraphDefaultUsesInitializedConstantStorage)
{
    auto execute = [](auto root) {
        std::vector<iv::Sample> output(16, 0.0f);
        runtime_output = output;
        auto executor = iv::BlockNodeExecutor::create(
            iv::TypeErasedNode(root), 8);
        executor.tick_block(0);
        executor.tick_block(8);
        runtime_output = {};
        return output;
    };

    auto const static_output = execute(static_default_sink_root);
    auto const runtime_output_values = execute(runtime_default_sink_root);
    for (size_t i = 0; i < static_output.size(); ++i) {
        EXPECT_EQ(static_output[i], iv::Sample{0.375f});
        EXPECT_EQ(runtime_output_values[i], iv::Sample{0.375f});
    }
}

// A graph cycle without detach() is now rejected while evaluating the consteval
// graph build. That diagnostic belongs in compile-fail coverage; it cannot be
// represented as a runtime EXPECT_THROW around GraphBuilder::build().
