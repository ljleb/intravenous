#include <intravenous/dsl.h>
#include <intravenous/graph_jit/connection_plan.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct TimedSamplePass {
    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_sample_input(
                "in", {}, iv::RealtimeInputConfig{.history = 5}),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::realtime_sample_output(
                "out", {}, iv::RealtimeOutputConfig{.history = 3, .latency = 2}),
        };
    }

    void tick_block(iv::TickBlockContext<TimedSamplePass> const&) const {}
};

struct PlainSamplePass {
    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<PlainSamplePass> const&) const {}
};

struct MonoSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<MonoSource> const&) const {}
};

struct LimitedMonoSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output("out")};
    }

    constexpr std::size_t max_block_size() const { return 16; }

    void tick_block(iv::TickBlockContext<LimitedMonoSource> const&) const {}
};

struct StereoSink {
    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_sample_input(
                "in",
                iv::SampleInputProperties{
                    .channel_layout = iv::ChannelLayout{
                        .channel_type = iv::ChannelTypeId::stereo,
                        .sample_layout = iv::SampleStreamLayout::planar,
                    },
                }),
        };
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<StereoSink> const&) const {}
};

struct CompiledSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::compiled_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<CompiledSource> const&) const {}
    void access_block_batch(iv::AccessBlockBatchContext<CompiledSource>&) const {}
};

struct CompiledSink {
    static constexpr auto inputs()
    {
        return std::array{iv::compiled_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<CompiledSink> const&) const {}
};

struct RealtimeSink {
    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<RealtimeSink> const&) const {}
};

struct LatentSamplePass {
    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output(
            "out", {}, iv::RealtimeOutputConfig{.latency = 2})};
    }

    constexpr std::size_t internal_latency() const { return 5; }

    void tick_block(iv::TickBlockContext<LatentSamplePass> const&) const {}
};

struct TwoInputSink {
    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_sample_input("fast"),
            iv::realtime_sample_input("slow"),
        };
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<TwoInputSink> const&) const {}
};

struct LatentTwoInputPass {
    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_sample_input("fast"),
            iv::realtime_sample_input("slow"),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output(
            "out", {}, iv::RealtimeOutputConfig{.latency = 4})};
    }

    constexpr std::size_t internal_latency() const { return 3; }

    void tick_block(iv::TickBlockContext<LatentTwoInputPass> const&) const {}
};

struct MediumLatencySamplePass {
    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output(
            "out", {}, iv::RealtimeOutputConfig{.latency = 1})};
    }

    constexpr std::size_t internal_latency() const { return 9; }

    void tick_block(iv::TickBlockContext<MediumLatencySamplePass> const&) const {}
};

} // namespace

TEST(GraphJitConnectionPlan, DerivesScheduleTemporalRequirementsAndProducerPolicy)
{
    using namespace iv;
    GraphBuilder graph;
    auto input = graph.input<"in">();
    auto first = details::configure_concrete_node<TimedSamplePass>(graph);
    auto second = details::configure_concrete_node<TimedSamplePass>(graph);
    auto const first_handle = first.node_bundle_handle();
    auto const second_handle = second.node_bundle_handle();
    first(input);
    second(first);
    graph.outputs("out"_P = second);

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    ASSERT_EQ(plan->nodes.size(), 2u);
    ASSERT_EQ(plan->dependencies.size(), 1u);
    EXPECT_EQ(plan->dependencies[0].source_bundle, first_handle);
    EXPECT_EQ(plan->dependencies[0].target_bundle, second_handle);
    EXPECT_EQ(
        plan->dependencies[0].payload,
        graph_jit::detail::PlannedConnectionPayload::sample);

    ASSERT_EQ(plan->schedule.regions.size(), 2u);
    ASSERT_EQ(plan->schedule.region_order.size(), 2u);
    EXPECT_FALSE(plan->schedule.regions[plan->schedule.region_order[0]].cyclic);
    EXPECT_FALSE(plan->schedule.regions[plan->schedule.region_order[1]].cyclic);
    ASSERT_TRUE(plan->schedule.bundle_execution_position[first_handle]);
    ASSERT_TRUE(plan->schedule.bundle_execution_position[second_handle]);
    EXPECT_LT(
        *plan->schedule.bundle_execution_position[first_handle],
        *plan->schedule.bundle_execution_position[second_handle]);

    auto const internal = std::ranges::find_if(
        plan->sample_connections,
        [&](graph_jit::detail::SampleConnectionPlan const& connection) {
            return connection.target_port.node_bundle_handle == second_handle;
        });
    ASSERT_NE(internal, plan->sample_connections.end());
    EXPECT_EQ(internal->source_history, 3u);
    EXPECT_EQ(internal->source_latency, 2u);
    EXPECT_EQ(internal->read_latency, 2u);
    EXPECT_EQ(internal->target_history, 5u);
    EXPECT_EQ(
        internal->access,
        graph_jit::detail::PlannedConnectionAccess::realtime_to_realtime);
    EXPECT_FALSE(internal->requires_conversion);
    EXPECT_FALSE(internal->external_boundary);
    EXPECT_FALSE(internal->feedback);

    auto const group = std::ranges::find_if(
        plan->sample_producer_groups,
        [&](graph_jit::detail::SampleProducerGroupPlan const& candidate) {
            return !candidate.source_channels.empty()
                && candidate.source_channels.front().bundle == first_handle;
        });
    ASSERT_NE(group, plan->sample_producer_groups.end());
    EXPECT_TRUE(group->has_realtime_connections);
    EXPECT_EQ(group->requirements.retained_frames, 7u);
    EXPECT_FALSE(group->requirements.direct_implementation_legal);
    ASSERT_TRUE(group->implementation.has_value());
    EXPECT_EQ(
        *group->implementation,
        SampleConnectionImplementationKind::compact_persistent_carry);

    auto const group_index = static_cast<std::size_t>(
        std::distance(plan->sample_producer_groups.begin(), group));
    EXPECT_TRUE(std::ranges::any_of(
        plan->storage.regions,
        [&](graph_jit::detail::ConnectionStorageRegionRequirement const& region) {
            return region.payload
                    == graph_jit::detail::PlannedConnectionPayload::sample
                && region.producer_group_index == group_index
                && region.lifetime
                    == graph_jit::detail::ConnectionStorageLifetime::persistent
                && region.current_block_frames == 0u
                && region.retained_extent == 7u
                && region.live_interval.crosses_kernel_invocations;
        }));
    EXPECT_TRUE(std::ranges::any_of(
        plan->storage.regions,
        [&](graph_jit::detail::ConnectionStorageRegionRequirement const& region) {
            return region.payload
                    == graph_jit::detail::PlannedConnectionPayload::sample
                && region.producer_group_index == group_index
                && region.lifetime
                    == graph_jit::detail::ConnectionStorageLifetime::transient
                && region.current_block_frames == 64u;
        }));
}

TEST(GraphJitConnectionPlan, EqualizesFeedForwardSamplePathsAtConvergence)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<MonoSource>(graph);
    auto latent = details::configure_concrete_node<LatentSamplePass>(graph);
    auto sink = details::configure_concrete_node<TwoInputSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    auto const latent_handle = latent.node_bundle_handle();
    auto const sink_handle = sink.node_bundle_handle();

    latent(source);
    sink("fast"_P = source, "slow"_P = latent);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    auto connection_to = [&](NodeBundleHandle source_bundle,
                             std::size_t target_port)
        -> graph_jit::detail::SampleConnectionPlan const* {
        auto const found = std::ranges::find_if(
            plan->sample_connections,
            [&](graph_jit::detail::SampleConnectionPlan const& connection) {
                return connection.target_port.node_bundle_handle == sink_handle
                    && connection.target_port.port_ordinal == target_port
                    && connection.canonical_source_port
                    && connection.canonical_source_port->node_bundle_handle
                        == source_bundle;
            });
        return found == plan->sample_connections.end() ? nullptr : &*found;
    };

    auto const* fast = connection_to(source_handle, 0);
    auto const* slow = connection_to(latent_handle, 1);
    ASSERT_NE(fast, nullptr);
    ASSERT_NE(slow, nullptr);

    // The slow path reaches the sink at 5 samples of node latency plus the
    // latent node's authored 2-sample output latency. The direct sibling must
    // therefore read seven samples behind while the slow path keeps only its
    // authored output latency.
    EXPECT_EQ(fast->source_latency, 0u);
    EXPECT_EQ(fast->read_latency, 7u);
    EXPECT_EQ(slow->source_latency, 2u);
    EXPECT_EQ(slow->read_latency, 2u);

    auto const source_group = std::ranges::find_if(
        plan->sample_producer_groups,
        [&](graph_jit::detail::SampleProducerGroupPlan const& group) {
            return !group.source_channels.empty()
                && group.source_channels.front().bundle == source_handle;
        });
    ASSERT_NE(source_group, plan->sample_producer_groups.end());
    EXPECT_EQ(source_group->requirements.retained_frames, 7u);
    ASSERT_TRUE(source_group->implementation.has_value());
    EXPECT_EQ(
        *source_group->implementation,
        SampleConnectionImplementationKind::compact_persistent_carry);
}

TEST(GraphJitConnectionPlan, PropagatesAlignedLatencyAcrossMultipleConvergences)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<MonoSource>(graph);
    auto first_slow = details::configure_concrete_node<LatentSamplePass>(graph);
    auto first_join = details::configure_concrete_node<LatentTwoInputPass>(graph);
    auto second_path = details::configure_concrete_node<MediumLatencySamplePass>(graph);
    auto sink = details::configure_concrete_node<TwoInputSink>(graph);

    auto const source_handle = source.node_bundle_handle();
    auto const first_slow_handle = first_slow.node_bundle_handle();
    auto const first_join_handle = first_join.node_bundle_handle();
    auto const second_path_handle = second_path.node_bundle_handle();
    auto const sink_handle = sink.node_bundle_handle();

    first_slow(source);
    first_join("fast"_P = source, "slow"_P = first_slow);
    second_path(source);
    sink("fast"_P = second_path, "slow"_P = first_join);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    auto connection_to = [&](NodeBundleHandle target_bundle,
                             std::size_t target_port,
                             NodeBundleHandle source_bundle)
        -> graph_jit::detail::SampleConnectionPlan const* {
        auto const found = std::ranges::find_if(
            plan->sample_connections,
            [&](graph_jit::detail::SampleConnectionPlan const& connection) {
                return connection.target_port.node_bundle_handle == target_bundle
                    && connection.target_port.port_ordinal == target_port
                    && connection.canonical_source_port
                    && connection.canonical_source_port->node_bundle_handle
                        == source_bundle;
            });
        return found == plan->sample_connections.end() ? nullptr : &*found;
    };

    auto const* first_fast = connection_to(first_join_handle, 0, source_handle);
    auto const* first_slow_connection =
        connection_to(first_join_handle, 1, first_slow_handle);
    auto const* second_fast = connection_to(sink_handle, 0, second_path_handle);
    auto const* second_slow = connection_to(sink_handle, 1, first_join_handle);
    ASSERT_NE(first_fast, nullptr);
    ASSERT_NE(first_slow_connection, nullptr);
    ASSERT_NE(second_fast, nullptr);
    ASSERT_NE(second_slow, nullptr);

    // First convergence: the LatentSamplePass path arrives at 5 internal + 2
    // authored output = 7 samples, so the direct source is delayed by 7.
    EXPECT_EQ(first_fast->source_latency, 0u);
    EXPECT_EQ(first_fast->read_latency, 7u);
    EXPECT_EQ(first_slow_connection->source_latency, 2u);
    EXPECT_EQ(first_slow_connection->read_latency, 2u);

    // The aligned first join therefore starts at path latency 7, adds its own
    // 3 samples of internal latency, then its authored output contributes 4
    // more at the second convergence: 7 + 3 + 4 = 14. The independent path
    // arrives at 9 internal + 1 authored output = 10, so only that path needs
    // four additional samples of compensation. Crucially, this proves the
    // first join propagates its aligned latency rather than resetting to its
    // local internal/output latency.
    EXPECT_EQ(second_slow->source_latency, 4u);
    EXPECT_EQ(second_slow->read_latency, 4u);
    EXPECT_EQ(second_fast->source_latency, 1u);
    EXPECT_EQ(second_fast->read_latency, 5u);

    auto const second_path_group = std::ranges::find_if(
        plan->sample_producer_groups,
        [&](graph_jit::detail::SampleProducerGroupPlan const& group) {
            return !group.source_channels.empty()
                && group.source_channels.front().bundle == second_path_handle;
        });
    ASSERT_NE(second_path_group, plan->sample_producer_groups.end());
    EXPECT_EQ(second_path_group->requirements.retained_frames, 5u);
    ASSERT_TRUE(second_path_group->implementation.has_value());
    EXPECT_EQ(
        *second_path_group->implementation,
        SampleConnectionImplementationKind::compact_persistent_carry);
}

TEST(GraphJitConnectionPlan, MarksCyclicProducerGroupsAsFeedback)
{
    using namespace iv;
    GraphBuilder graph;
    auto first = details::configure_concrete_node<PlainSamplePass>(graph);
    auto second = details::configure_concrete_node<PlainSamplePass>(graph);
    auto const first_handle = first.node_bundle_handle();
    auto const second_handle = second.node_bundle_handle();
    first(second);
    second(first);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    ASSERT_EQ(plan->schedule.regions.size(), 1u);
    EXPECT_TRUE(plan->schedule.regions.front().cyclic);
    EXPECT_EQ(
        plan->schedule.regions.front().nodes,
        (std::vector<NodeBundleHandle>{first_handle, second_handle}));
    ASSERT_EQ(plan->sample_connections.size(), 2u);
    EXPECT_TRUE(std::ranges::all_of(
        plan->sample_connections,
        &graph_jit::detail::SampleConnectionPlan::feedback));
    ASSERT_EQ(plan->sample_producer_groups.size(), 2u);
    for (auto const& group : plan->sample_producer_groups) {
        ASSERT_TRUE(group.implementation.has_value());
        EXPECT_EQ(
            *group.implementation,
            SampleConnectionImplementationKind::feedback_ring);
        EXPECT_TRUE(group.requirements.feedback);
    }
}

TEST(GraphJitConnectionPlan, SimpleRealtimeSampleEdgeChoosesDirect)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<MonoSource>(graph);
    auto sink = details::configure_concrete_node<RealtimeSink>(graph);
    sink(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    ASSERT_EQ(plan->sample_connections.size(), 1u);
    EXPECT_FALSE(plan->sample_connections[0].requires_conversion);
    EXPECT_FALSE(plan->sample_connections[0].requires_block_materialization);
    ASSERT_EQ(plan->sample_producer_groups.size(), 1u);
    auto const& group = plan->sample_producer_groups[0];
    EXPECT_TRUE(group.requirements.direct_implementation_legal);
    EXPECT_FALSE(group.requirements.requires_materialization);
    EXPECT_EQ(group.requirements.retained_frames, 0u);
    ASSERT_TRUE(group.implementation.has_value());
    EXPECT_EQ(*group.implementation, SampleConnectionImplementationKind::direct);
    EXPECT_TRUE(plan->storage.regions.empty());
}

TEST(GraphJitConnectionPlan, ConversionUsesTransientMaterialization)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<MonoSource>(graph);
    auto sink = details::configure_concrete_node<StereoSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    auto const sink_handle = sink.node_bundle_handle();
    sink(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 128);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    ASSERT_EQ(plan->sample_connections.size(), 1u);
    EXPECT_TRUE(plan->sample_connections[0].requires_conversion);
    EXPECT_EQ(plan->sample_connections[0].source_type, ChannelTypeId::mono);
    EXPECT_EQ(plan->sample_connections[0].target_type, ChannelTypeId::stereo);

    ASSERT_EQ(plan->sample_producer_groups.size(), 1u);
    auto const& group = plan->sample_producer_groups[0];
    EXPECT_TRUE(group.requirements.requires_materialization);
    EXPECT_EQ(group.requirements.retained_frames, 0u);
    ASSERT_TRUE(group.implementation.has_value());
    EXPECT_EQ(
        *group.implementation,
        SampleConnectionImplementationKind::transient_materialization);

    ASSERT_EQ(plan->storage.regions.size(), 1u);
    auto const& storage = plan->storage.regions[0];
    EXPECT_EQ(
        storage.lifetime,
        graph_jit::detail::ConnectionStorageLifetime::transient);
    EXPECT_EQ(storage.current_block_frames, 128u);
    ASSERT_TRUE(plan->schedule.bundle_execution_position[source_handle]);
    ASSERT_TRUE(plan->schedule.bundle_execution_position[sink_handle]);
    EXPECT_EQ(
        storage.live_interval.begin,
        *plan->schedule.bundle_execution_position[source_handle]);
    EXPECT_EQ(
        storage.live_interval.end,
        *plan->schedule.bundle_execution_position[sink_handle]);
}

TEST(GraphJitConnectionPlan, ExternalEventsUseBoundaryPolicy)
{
    using namespace iv;
    GraphBuilder graph;
    auto event = graph.event_input<"event">(EventTypeId::trigger);
    graph.event_outputs("event"_F = event);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    ASSERT_EQ(plan->event_connections.size(), 1u);
    EXPECT_TRUE(plan->event_connections[0].external_boundary);
    EXPECT_EQ(
        plan->event_connections[0].access,
        graph_jit::detail::PlannedConnectionAccess::realtime_to_realtime);
    ASSERT_EQ(plan->event_producer_groups.size(), 1u);
    auto const& group = plan->event_producer_groups[0];
    ASSERT_TRUE(group.implementation.has_value());
    EXPECT_EQ(
        *group.implementation,
        EventConnectionImplementationKind::external_boundary);
}

TEST(GraphJitConnectionPlan, BlockSliceMismatchRequiresMaterialization)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<LimitedMonoSource>(graph);
    auto sink = details::configure_concrete_node<RealtimeSink>(graph);
    sink(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    ASSERT_EQ(plan->sample_connections.size(), 1u);
    EXPECT_TRUE(plan->sample_connections[0].requires_block_materialization);
    ASSERT_EQ(plan->sample_producer_groups.size(), 1u);
    auto const& group = plan->sample_producer_groups[0];
    EXPECT_TRUE(group.requirements.requires_materialization);
    EXPECT_FALSE(group.requirements.direct_implementation_legal);
    ASSERT_TRUE(group.implementation.has_value());
    EXPECT_EQ(
        *group.implementation,
        SampleConnectionImplementationKind::transient_materialization);
}

TEST(GraphJitConnectionPlan, CompiledConnectionsDoNotUseRealtimeStoragePolicy)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<CompiledSource>(graph);
    auto sink = details::configure_concrete_node<CompiledSink>(graph);
    sink(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    ASSERT_EQ(plan->sample_connections.size(), 1u);
    EXPECT_EQ(
        plan->sample_connections[0].access,
        graph_jit::detail::PlannedConnectionAccess::compiled_to_compiled);
    ASSERT_EQ(plan->dependencies.size(), 1u);
    EXPECT_FALSE(plan->dependencies[0].sequential_tick_dependency);
    ASSERT_EQ(plan->sample_producer_groups.size(), 1u);
    EXPECT_FALSE(plan->sample_producer_groups[0].has_realtime_connections);
    EXPECT_TRUE(plan->sample_producer_groups[0].has_compiled_connections);
    EXPECT_FALSE(plan->sample_producer_groups[0].implementation.has_value());
    EXPECT_TRUE(plan->storage.regions.empty());
}

TEST(GraphJitConnectionPlan, PreservesCompiledRealtimeAccessDirection)
{
    using namespace iv;

    GraphBuilder compiled_to_realtime;
    auto compiled_source =
        details::configure_concrete_node<CompiledSource>(compiled_to_realtime);
    auto realtime_sink =
        details::configure_concrete_node<RealtimeSink>(compiled_to_realtime);
    realtime_sink(compiled_source);
    compiled_to_realtime.outputs();

    auto compiled_to_realtime_graph = std::move(compiled_to_realtime).finish();
    auto compiled_to_realtime_plan =
        graph_jit::detail::build_connection_analysis_plan(
            compiled_to_realtime_graph, 64);
    ASSERT_TRUE(compiled_to_realtime_plan.has_value())
        << (compiled_to_realtime_plan
                ? std::string{}
                : compiled_to_realtime_plan.error());
    ASSERT_EQ(compiled_to_realtime_plan->sample_connections.size(), 1u);
    EXPECT_EQ(
        compiled_to_realtime_plan->sample_connections[0].access,
        graph_jit::detail::PlannedConnectionAccess::compiled_to_realtime);
    ASSERT_EQ(compiled_to_realtime_plan->dependencies.size(), 1u);
    EXPECT_FALSE(
        compiled_to_realtime_plan->dependencies[0].sequential_tick_dependency);
    ASSERT_EQ(compiled_to_realtime_plan->sample_producer_groups.size(), 1u);
    EXPECT_FALSE(
        compiled_to_realtime_plan->sample_producer_groups[0]
            .has_realtime_connections);
    EXPECT_TRUE(
        compiled_to_realtime_plan->sample_producer_groups[0]
            .has_compiled_connections);
    EXPECT_FALSE(
        compiled_to_realtime_plan->sample_producer_groups[0]
            .implementation.has_value());

    GraphBuilder realtime_to_compiled;
    auto realtime_source =
        details::configure_concrete_node<MonoSource>(realtime_to_compiled);
    auto compiled_sink =
        details::configure_concrete_node<CompiledSink>(realtime_to_compiled);
    compiled_sink(realtime_source);
    realtime_to_compiled.outputs();

    auto realtime_to_compiled_graph = std::move(realtime_to_compiled).finish();
    auto realtime_to_compiled_plan =
        graph_jit::detail::build_connection_analysis_plan(
            realtime_to_compiled_graph, 64);
    ASSERT_TRUE(realtime_to_compiled_plan.has_value())
        << (realtime_to_compiled_plan
                ? std::string{}
                : realtime_to_compiled_plan.error());
    ASSERT_EQ(realtime_to_compiled_plan->sample_connections.size(), 1u);
    EXPECT_EQ(
        realtime_to_compiled_plan->sample_connections[0].access,
        graph_jit::detail::PlannedConnectionAccess::realtime_to_compiled);
    ASSERT_EQ(realtime_to_compiled_plan->dependencies.size(), 1u);
    EXPECT_TRUE(
        realtime_to_compiled_plan->dependencies[0].sequential_tick_dependency);
    ASSERT_EQ(realtime_to_compiled_plan->sample_producer_groups.size(), 1u);
    EXPECT_FALSE(
        realtime_to_compiled_plan->sample_producer_groups[0]
            .has_realtime_connections);
    EXPECT_TRUE(
        realtime_to_compiled_plan->sample_producer_groups[0]
            .has_compiled_connections);
    EXPECT_FALSE(
        realtime_to_compiled_plan->sample_producer_groups[0]
            .implementation.has_value());
}
