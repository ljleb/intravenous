#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/dsl.h>
#include <configured_graph_test_view.h>
#include <intravenous/graph/builder/embedder.hpp>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>
#include <intravenous/node/block_executor.h>

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <vector>

namespace {
    using namespace iv;

    std::span<iv::Sample> runtime_output {};

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

TEST(DetachRegression, BuilderSessionStoresDetachOnConcreteConnections)
{
    iv::GraphBuilder graph;

    auto const sample_source = iv::details::configure_concrete_node<iv::Constant>(
        graph, iv::Sample{0.5f});
    auto const sample_sink_a = iv::details::configure_concrete_node<
        iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>>(graph);
    auto const sample_sink_b = iv::details::configure_concrete_node<
        iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>>(graph);
    sample_sink_a(static_cast<iv::SamplePortRef>(sample_source).detach(
        5, iv::Sample{0.25f}));
    sample_sink_b(static_cast<iv::SamplePortRef>(sample_source).detach(
        9, iv::Sample{-0.5f}));

    auto const event_source =
        iv::details::configure_concrete_node<DetachedEventSource>(graph);
    auto const event_sink_a =
        iv::details::configure_concrete_node<DetachedEventSink>(graph);
    auto const event_sink_b =
        iv::details::configure_concrete_node<DetachedEventSink>(graph);
    event_sink_a(event_source.event_port().detach(7));
    event_sink_b(event_source.event_port().detach(11));

    auto detached = event_source.event_port().detach(13);
    EXPECT_ANY_THROW(detached.detach(14));

    auto configured = std::move(graph).finish();
    // Boundary + six authored nodes only: detach no longer materializes hidden
    // writer/reader node bundles in configured graph state.
    EXPECT_EQ(configured.node_bundles.size(), 7u);
    auto const samples = configured.connections.configured_sample_connections();
    ASSERT_EQ(samples.size(), 2u);
    ASSERT_TRUE(samples[0].detach.has_value());
    ASSERT_TRUE(samples[1].detach.has_value());
    EXPECT_EQ(samples[0].detach->loop_extra_latency, 5u);
    EXPECT_EQ(samples[1].detach->loop_extra_latency, 9u);
    ASSERT_TRUE(samples[0].detach->initial_value_override.has_value());
    ASSERT_TRUE(samples[1].detach->initial_value_override.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(*samples[0].detach->initial_value_override), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(*samples[1].detach->initial_value_override), -0.5f);

    auto const events = configured.connections.configured_event_connections();
    ASSERT_EQ(events.size(), 2u);
    ASSERT_TRUE(events[0].detach.has_value());
    ASSERT_TRUE(events[1].detach.has_value());
    EXPECT_EQ(events[0].detach->loop_extra_latency, 7u);
    EXPECT_EQ(events[1].detach->loop_extra_latency, 11u);

}

TEST(DetachRegression, ChannelProjectionPreservesDetachMetadata)
{
    iv::GraphBuilder graph;
    auto const input = graph.input<"input", iv::stereo>();
    auto const detached = static_cast<iv::SamplePortRef>(input).detach(
        7, iv::Sample{-0.25f});

    constexpr std::size_t projection_count = 64;
    for (std::size_t i = 0; i < projection_count; ++i) {
        auto const sink = iv::details::configure_concrete_node<
            iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>>(graph);
        sink(detached.select_channel(i % 2));
    }
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto const connections =
        configured.connections.configured_sample_connections();
    ASSERT_EQ(connections.size(), projection_count);
    for (std::size_t i = 0; i < connections.size(); ++i) {
        auto const& connection = connections[i];
        ASSERT_TRUE(connection.detach.has_value());
        EXPECT_EQ(connection.detach->loop_extra_latency, 7u);
        ASSERT_TRUE(connection.detach->initial_value_override.has_value());
        EXPECT_FLOAT_EQ(
            static_cast<float>(*connection.detach->initial_value_override),
            -0.25f);
        ASSERT_EQ(connection.source_channels.size(), 1u);
        EXPECT_EQ(connection.source_channels.front().channel, i % 2);
    }
}

TEST(DetachRegression, TilingRequiresMatchingDetachMetadata)
{
    iv::GraphBuilder graph;
    auto const left = iv::details::configure_concrete_node<iv::Constant>(
        graph, iv::Sample{0.25f});
    auto const right = iv::details::configure_concrete_node<iv::Constant>(
        graph, iv::Sample{-0.5f});
    auto const detached_left = static_cast<iv::SamplePortRef>(left).detach(
        9, iv::Sample{0.125f});
    auto const detached_right = static_cast<iv::SamplePortRef>(right).detach(
        9, iv::Sample{0.125f});
    auto const tiled = graph.tile<iv::stereo>(detached_left, detached_right);
    graph.outputs(tiled);

    auto configured = std::move(graph).finish();
    auto const connections =
        configured.connections.configured_sample_connections();
    ASSERT_EQ(connections.size(), 1u);
    ASSERT_TRUE(connections.front().detach.has_value());
    EXPECT_EQ(connections.front().detach->loop_extra_latency, 9u);
    ASSERT_TRUE(
        connections.front().detach->initial_value_override.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(
            *connections.front().detach->initial_value_override),
        0.125f);

    iv::GraphBuilder invalid_graph;
    auto const invalid_left =
        iv::details::configure_concrete_node<iv::Constant>(
            invalid_graph, iv::Sample{0.25f});
    auto const invalid_right =
        iv::details::configure_concrete_node<iv::Constant>(
            invalid_graph, iv::Sample{-0.5f});
    EXPECT_ANY_THROW(invalid_graph.tile<iv::stereo>(
        static_cast<iv::SamplePortRef>(invalid_left).detach(5),
        static_cast<iv::SamplePortRef>(invalid_right).detach(7)));
}

TEST(DetachRegression, ChildImportRemapsDetachedConnectionEndpoints)
{
    iv::GraphBuilder parent_builder;
    iv::details::configure_concrete_node<iv::Constant>(
        parent_builder, iv::Sample{0.0f});
    parent_builder.outputs();
    auto parent = std::move(parent_builder).finish();

    iv::GraphBuilder child_builder;
    auto const source = iv::details::configure_concrete_node<iv::Constant>(
        child_builder, iv::Sample{0.5f});
    auto const sink = iv::details::configure_concrete_node<
        iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>>(child_builder);
    sink(static_cast<iv::SamplePortRef>(source).detach(
        6, iv::Sample{-0.75f}));
    child_builder.outputs();
    auto child = std::move(child_builder).finish();

    ASSERT_EQ(
        child.connections.configured_sample_connections().size(), 1u);
    auto const child_connection =
        child.connections.configured_sample_connections().front();
    auto const imported = iv::GraphBuilderChildEmbedder::import(
        parent.node_bundles,
        parent.connections,
        parent.virtual_nodes,
        child.node_bundles,
        child.connections,
        child.virtual_nodes);

    auto const connections =
        parent.connections.configured_sample_connections();
    ASSERT_EQ(connections.size(), 1u);
    auto const& connection = connections.front();
    ASSERT_EQ(connection.source_channels.size(), 1u);
    ASSERT_EQ(connection.target_channels.size(), 1u);
    EXPECT_EQ(
        connection.source_channels.front().bundle,
        child_connection.source_channels.front().bundle
            + imported.bundle_offset);
    EXPECT_EQ(
        connection.target_channels.front().bundle,
        child_connection.target_channels.front().bundle
            + imported.bundle_offset);
    ASSERT_TRUE(connection.detach.has_value());
    EXPECT_EQ(connection.detach->loop_extra_latency, 6u);
    ASSERT_TRUE(connection.detach->initial_value_override.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(*connection.detach->initial_value_override),
        -0.75f);
}
