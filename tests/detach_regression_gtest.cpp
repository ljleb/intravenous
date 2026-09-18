#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/dsl.h>
#include <configured_graph_test_view.h>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>
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
            return std::array {iv::realtime_sample_output("value")};
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

    struct DetachedEventSource {
        static constexpr auto outputs()
        {
            return std::array{iv::realtime_event_output(
                "trigger",
                iv::EventOutputProperties{
                    .type = iv::EventTypeId::trigger,
                    .max_events_per_sample = 0.25,
                })};
        }

        void tick_block(iv::TickBlockContext<DetachedEventSource> const&) const {}
    };

    struct DetachedEventSink {
        static constexpr auto inputs()
        {
            return std::array{
                iv::realtime_event_input("trigger", iv::EventTypeId::trigger),
            };
        }

        void tick_block(iv::TickBlockContext<DetachedEventSink> const&) const {}
    };

    void detached_voice(
        iv::GraphBuilder& g,
        iv::SubgraphBuilder& boundary,
        iv::SamplePortRef dt,
        iv::SamplePortRef noise,
        iv::Sample amplitude)
    {
        auto const reset = 1.0f;
        auto const frequency = 220.0f;
        auto const integrator = details::configure_concrete_node<iv::PhaseIntegrator>(g);
        auto const warper = details::configure_concrete_node<iv::Warper>(g);

        integrator((warper["aliased"].detach() * reset + frequency * 2.0f) * dt);
        warper(integrator + noise);
        boundary.outputs("out"_P = (warper["anti_aliased"] * amplitude));
    }

    auto build_detached_graph()
    {
        iv::GraphBuilder graph;
        auto const dt = details::configure_concrete_node<RuntimeValueSource>(
            graph, RuntimeValueSlot::dt);
        auto const src_a = details::configure_concrete_node<RuntimeValueSource>(
            graph, RuntimeValueSlot::noise_a);
        auto const src_b = details::configure_concrete_node<RuntimeValueSource>(
            graph, RuntimeValueSlot::noise_b);
        auto const voice_a = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
            detached_voice(graph, boundary, dt, src_a, 0.5f);
        });
        auto const voice_b = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
            detached_voice(graph, boundary, dt, src_b, 0.25f);
        });
        auto const sink = details::configure_concrete_node<RuntimeBufferSink>(graph);

        sink(voice_a + voice_b);
        graph.outputs();
        return iv::freeze_configured_graph_for_test(std::move(graph).finish());
    }

    iv::RuntimeGraphRoot build_runtime_root(
        iv::ConfiguredGraphTestView view,
        bool execution_root = false)
    {
        auto configured = iv::thaw_configured_graph_for_test(view);
        auto plan = iv::GraphCompiler::compile(
            iv::GraphLowerer::lower(
                std::move(configured), {.execution_root = execution_root}));
        return iv::RuntimeGraphRoot(std::move(plan.graph));
    }

    iv::RuntimeGraphRoot build_detached_runtime_root()
    {
        static const auto view = build_detached_graph();
        return build_runtime_root(view);
    }

    auto build_static_dormancy_graph()
    {
        iv::GraphBuilder graph;
        auto const source = details::configure_concrete_node<iv::Constant>(
            graph, iv::Sample{0.0f});
        auto const nested = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
            auto const input = boundary.input<"in">(0.0f);
            auto const pass = details::configure_concrete_node<
                iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>>(graph);
            pass(input);
            boundary.outputs("out"_P = pass);
        }).ttl(1);
        nested("in"_P = source);
        graph.outputs("out"_P = nested);
        return iv::freeze_configured_graph_for_test(std::move(graph).finish());
    }

    iv::RuntimeGraphRoot build_dormancy_runtime_root()
    {
        static const auto view = build_static_dormancy_graph();
        return build_runtime_root(view, true);
    }

    // An unconnected subgraph input lowers through materialize_subgraph_default.
    // Its default must reach a block-reading node without leaving a ticking
    // Constant wrapper in the executable graph.
    auto build_static_default_sink_graph()
    {
        iv::GraphBuilder graph;
        auto const nested = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
            auto const input = boundary.input<"in">(iv::Sample{0.375f});
            auto const pass = details::configure_concrete_node<
                iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>>(graph);
            pass(input);
            boundary.outputs("out"_P = pass);
        });
        auto const sink = details::configure_concrete_node<RuntimeBufferSink>(graph);
        sink(nested);
        graph.outputs();
        return iv::freeze_configured_graph_for_test(std::move(graph).finish());
    }

    iv::RuntimeGraphRoot build_default_sink_runtime_root()
    {
        static const auto view = build_static_default_sink_graph();
        return build_runtime_root(view);
    }

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
        iv::TypeErasedNode(build_detached_runtime_root()),
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

TEST(DetachRegression, RuntimeGraphRootIsDeterministic)
{
    runtime_values[static_cast<size_t>(RuntimeValueSlot::dt)] =
        iv::Sample{1.0f / 48000.0f};
    runtime_values[static_cast<size_t>(RuntimeValueSlot::noise_a)] =
        iv::Sample{0.125f};
    runtime_values[static_cast<size_t>(RuntimeValueSlot::noise_b)] =
        iv::Sample{-0.25f};

    std::vector<iv::Sample> first_output(32, 0.0f);
    runtime_output = first_output;
    auto first_executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(build_detached_runtime_root()), first_output.size());
    tick_executor_direct(first_executor, 0, first_output.size());

    std::vector<iv::Sample> runtime_root_output(32, 0.0f);
    runtime_output = runtime_root_output;
    auto runtime_executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(build_detached_runtime_root()), runtime_root_output.size());
    tick_executor_direct(runtime_executor, 0, runtime_root_output.size());
    runtime_output = {};

    ASSERT_EQ(runtime_root_output.size(), first_output.size());
    for (size_t i = 0; i < first_output.size(); ++i) {
        EXPECT_EQ(runtime_root_output[i], first_output[i])
            << "output differs at sample " << i;
    }
}

TEST(DetachRegression, RuntimeGraphRootExecutesDormancyGroups)
{
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(build_dormancy_runtime_root()), 8);

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

    auto const first_output = execute(build_default_sink_runtime_root());
    auto const runtime_output_values = execute(build_default_sink_runtime_root());
    for (size_t i = 0; i < first_output.size(); ++i) {
        EXPECT_EQ(first_output[i], iv::Sample{0.375f});
        EXPECT_EQ(runtime_output_values[i], iv::Sample{0.375f});
    }
}

TEST(DetachRegression, BuilderSessionPreservesSampleAndEventDetachLatency)
{
    iv::GraphBuilder graph;

    auto const sample_source = iv::details::configure_concrete_node<iv::Constant>(
        graph, iv::Sample{0.5f});
    auto const sample_detached =
        static_cast<iv::SamplePortRef>(sample_source).detach(5);
    (void)sample_detached;

    auto const event_source =
        iv::details::configure_concrete_node<DetachedEventSource>(graph);
    auto const event_sink =
        iv::details::configure_concrete_node<DetachedEventSink>(graph);
    auto const event_port = event_source.event_port();
    auto const event_sources = event_port.sources();
    auto const original_event_sources =
        std::vector<iv::EventOutputPortId>(
            event_sources.begin(), event_sources.end());

    auto const event_detached = event_port.detach(7);
    auto const event_detached_again = event_detached.detach(7);
    EXPECT_EQ(
        std::vector<iv::EventOutputPortId>(
            event_detached.sources().begin(), event_detached.sources().end()),
        std::vector<iv::EventOutputPortId>(
            event_detached_again.sources().begin(),
            event_detached_again.sources().end()));
    EXPECT_ANY_THROW(event_source.event_port().detach(8));
    event_sink(event_detached);

    auto configured = std::move(graph).finish();
    ASSERT_EQ(configured.detach.next_detach_id(), 2u);

    auto const sample_infos = configured.detach.configured_infos();
    ASSERT_EQ(sample_infos.size(), 1u);
    EXPECT_EQ(sample_infos[0].loop_extra_latency, 5u);

    auto const event_infos = configured.detach.configured_event_infos();
    ASSERT_EQ(event_infos.size(), 1u);
    EXPECT_EQ(event_infos[0].detach_id, 1u);
    EXPECT_EQ(event_infos[0].source_type, iv::EventTypeId::trigger);
    EXPECT_EQ(event_infos[0].sources, original_event_sources);
    EXPECT_EQ(event_infos[0].loop_extra_latency, 7u);
    EXPECT_EQ(event_infos[0].reader_port.bundle, event_infos[0].reader_bundle);
    EXPECT_EQ(event_infos[0].reader_port.port, 0u);
}
