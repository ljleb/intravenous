#include <intravenous/dsl.h>
#include <intravenous/graph_jit/connection_plan.h>
#include <intravenous/graph_jit/sample_physical_plan.h>

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

struct RealtimeRecorderPass {
    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::realtime_sample_output("realtime"),
            iv::indexed_sample_output(
                "recorded",
                {},
                {.producer = iv::IndexedProducer::tick_record}),
        };
    }

    void tick_block(iv::TickBlockContext<RealtimeRecorderPass> const&) const {}
};

struct NeutralSamplePass {
    static constexpr auto inputs()
    {
        return std::array{iv::realtime_sample_input(
            "in", iv::SampleInputProperties{.neutral_value = 0.375f})};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<NeutralSamplePass> const&) const {}
};

struct PlainEventPass {
    static constexpr auto inputs()
    {
        return std::array{
            iv::realtime_event_input("in", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::realtime_event_output(
            "out",
            iv::EventOutputProperties{
                .type = iv::EventTypeId::trigger,
                .max_events_per_index = 0.25,
            })};
    }

    void tick_block(iv::TickBlockContext<PlainEventPass> const&) const {}
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

struct IndexedSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::indexed_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<IndexedSource> const&) const {}
    void tock_coverage(iv::TockCoverageContext<IndexedSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<IndexedSource>& context) const
    {
        context.template output<"out">().publish_coverage({});
    }
};

struct IndexedSamplePass {
    static constexpr auto inputs()
    {
        return std::array{iv::indexed_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::indexed_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<IndexedSamplePass> const&) const {}
    void tock_coverage(iv::TockCoverageContext<IndexedSamplePass>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<IndexedSamplePass>& context) const
    {
        context.template output<"out">().publish_coverage(
            context.template input<"in">().coverage());
    }
};

struct IndexedEventPass {
    static constexpr auto inputs()
    {
        return std::array{
            iv::indexed_event_input("in", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::indexed_event_output("out", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<IndexedEventPass> const&) const {}
    void tock_coverage(iv::TockCoverageContext<IndexedEventPass>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<IndexedEventPass>& context) const
    {
        context.template output<"out">().publish_coverage(
            context.template input<"in">().coverage());
    }
};

struct IndexedSink {
    static constexpr auto inputs()
    {
        return std::array{iv::indexed_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<IndexedSink> const&) const {}
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
    EXPECT_FALSE(internal->detach.has_value());

    auto const group = std::ranges::find_if(
        plan->sample_producer_groups,
        [&](graph_jit::detail::SampleProducerGroupPlan const& candidate) {
            return !candidate.source_channels.empty()
                && candidate.source_channels.front().bundle == first_handle;
        });
    ASSERT_NE(group, plan->sample_producer_groups.end());
    EXPECT_TRUE(group->has_realtime_connections);
    EXPECT_EQ(group->storage_requirements.retained_frames, 7u);
    ASSERT_TRUE(group->storage_plan.has_value());
    EXPECT_EQ(
        group->storage_plan->kind,
        RealtimeBufferStorageKind::stack_with_persistent_carry);

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
    EXPECT_EQ(source_group->storage_requirements.retained_frames, 7u);
    ASSERT_TRUE(source_group->storage_plan.has_value());
    EXPECT_EQ(
        source_group->storage_plan->kind,
        RealtimeBufferStorageKind::stack_with_persistent_carry);
}

TEST(GraphJitConnectionPlan, EqualizesChannelsInsideComposedSampleInput)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<MonoSource>(graph);
    auto latent = details::configure_concrete_node<LatentSamplePass>(graph);
    auto sink = details::configure_concrete_node<StereoSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    auto const latent_handle = latent.node_bundle_handle();
    auto const sink_handle = sink.node_bundle_handle();

    latent(source);
    sink(graph.tile<stereo>(source, latent));
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    auto const connection = std::ranges::find_if(
        plan->sample_connections,
        [&](graph_jit::detail::SampleConnectionPlan const& candidate) {
            return candidate.target_port.node_bundle_handle == sink_handle;
        });
    ASSERT_NE(connection, plan->sample_connections.end());
    EXPECT_FALSE(connection->canonical_source_port.has_value());
    ASSERT_EQ(connection->source_channel_timings.size(), 2u);

    auto const& fast = connection->source_channel_timings[0];
    auto const& slow = connection->source_channel_timings[1];
    EXPECT_EQ(fast.source.bundle, source_handle);
    EXPECT_EQ(slow.source.bundle, latent_handle);

    // The left channel comes directly from the zero-latency source. The right
    // channel comes from a node with 5 samples of internal latency and 2
    // samples of authored output latency. Both channels belong to one stereo
    // input connection, but they still require independent temporal reads.
    EXPECT_EQ(fast.source_latency, 0u);
    EXPECT_EQ(fast.read_latency, 7u);
    EXPECT_EQ(slow.source_latency, 2u);
    EXPECT_EQ(slow.read_latency, 2u);

    // The scalar is only a conservative compatibility value for the current
    // whole-port storage planner. Physical channel composition must consume
    // the per-channel timings above instead of applying this value uniformly.
    EXPECT_EQ(connection->source_latency, 2u);
    EXPECT_EQ(connection->read_latency, 7u);

    auto const source_group = std::ranges::find_if(
        plan->sample_producer_groups,
        [&](graph_jit::detail::SampleProducerGroupPlan const& group) {
            return group.source_port
                && group.source_port->node_bundle_handle == source_handle;
        });
    auto const latent_group = std::ranges::find_if(
        plan->sample_producer_groups,
        [&](graph_jit::detail::SampleProducerGroupPlan const& group) {
            return group.source_port
                && group.source_port->node_bundle_handle == latent_handle;
        });
    ASSERT_NE(source_group, plan->sample_producer_groups.end());
    ASSERT_NE(latent_group, plan->sample_producer_groups.end());
    EXPECT_EQ(source_group->storage_requirements.retained_frames, 7u);
    EXPECT_EQ(latent_group->storage_requirements.retained_frames, 2u);

    auto physical = graph_jit::detail::build_sample_physical_plan(*plan, 64);
    ASSERT_TRUE(physical.has_value())
        << (physical ? std::string{} : physical.error());
    EXPECT_TRUE(physical->compositions.empty());
    auto const connection_index = static_cast<std::size_t>(std::distance(
        plan->sample_connections.begin(), connection));
    ASSERT_LT(connection_index, physical->connection_representations.size());
    ASSERT_LT(connection_index, physical->connection_channel_bindings.size());
    EXPECT_FALSE(physical->connection_representations[connection_index].has_value());
    ASSERT_TRUE(physical->connection_channel_bindings[connection_index].has_value());

    auto const& aliases =
        *physical->connection_channel_bindings[connection_index];
    ASSERT_EQ(aliases.size(), 2u);
    auto const source_group_index = static_cast<std::size_t>(std::distance(
        plan->sample_producer_groups.begin(), source_group));
    auto const latent_group_index = static_cast<std::size_t>(std::distance(
        plan->sample_producer_groups.begin(), latent_group));
    ASSERT_TRUE(physical->producer_groups[source_group_index].has_value());
    ASSERT_TRUE(physical->producer_groups[latent_group_index].has_value());
    EXPECT_EQ(
        aliases[0].representation,
        physical->producer_groups[source_group_index]->canonical_representation);
    EXPECT_EQ(aliases[0].representation_channel, 0u);
    EXPECT_EQ(aliases[0].frame_delay, 7u);
    EXPECT_EQ(
        aliases[1].representation,
        physical->producer_groups[latent_group_index]->canonical_representation);
    EXPECT_EQ(aliases[1].representation_channel, 0u);
    EXPECT_EQ(aliases[1].frame_delay, 2u);

    ASSERT_LT(sink_handle, plan->schedule.bundle_execution_position.size());
    ASSERT_TRUE(plan->schedule.bundle_execution_position[sink_handle].has_value());
    auto const sink_position =
        *plan->schedule.bundle_execution_position[sink_handle];
    EXPECT_GE(
        physical->representations[aliases[0].representation].live_interval.end,
        sink_position);
    EXPECT_GE(
        physical->representations[aliases[1].representation].live_interval.end,
        sink_position);
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
    EXPECT_EQ(second_path_group->storage_requirements.retained_frames, 5u);
    ASSERT_TRUE(second_path_group->storage_plan.has_value());
    EXPECT_EQ(
        second_path_group->storage_plan->kind,
        RealtimeBufferStorageKind::stack_with_persistent_carry);
}

TEST(GraphJitConnectionPlan, RejectsImplicitCycles)
{
    using namespace iv;
    GraphBuilder graph;
    auto first = details::configure_concrete_node<PlainSamplePass>(graph);
    auto second = details::configure_concrete_node<PlainSamplePass>(graph);
    first(second);
    second(first);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_FALSE(plan.has_value());
    EXPECT_NE(plan.error().find("implicit cycle"), std::string::npos);

    GraphBuilder event_graph;
    auto event_first = details::configure_concrete_node<PlainEventPass>(event_graph);
    auto event_second = details::configure_concrete_node<PlainEventPass>(event_graph);
    event_first.connect_event_input(0, event_second.event_port());
    event_second.connect_event_input(0, event_first.event_port());
    event_graph.outputs();

    auto event_configured = std::move(event_graph).finish();
    auto event_plan = graph_jit::detail::build_connection_analysis_plan(
        event_configured, 64);
    ASSERT_FALSE(event_plan.has_value());
    EXPECT_NE(event_plan.error().find("implicit cycle"), std::string::npos);
}

TEST(GraphJitConnectionPlan, RejectsDetachThatDoesNotBreakCycle)
{
    using namespace iv;
    GraphBuilder sample_graph;
    auto sample_source = details::configure_concrete_node<MonoSource>(sample_graph);
    auto sample_sink = details::configure_concrete_node<RealtimeSink>(sample_graph);
    sample_sink(static_cast<SamplePortRef>(sample_source).detach(5));
    sample_graph.outputs();

    auto sample_configured = std::move(sample_graph).finish();
    auto sample_plan = graph_jit::detail::build_connection_analysis_plan(
        sample_configured, 64);
    ASSERT_FALSE(sample_plan.has_value());
    EXPECT_NE(
        sample_plan.error().find("breaks an acyclic dependency"),
        std::string::npos);

    GraphBuilder event_graph;
    auto event_source = details::configure_concrete_node<PlainEventPass>(event_graph);
    auto event_sink = details::configure_concrete_node<PlainEventPass>(event_graph);
    event_sink.connect_event_input(0, event_source.event_port().detach(7));
    event_graph.outputs();

    auto event_configured = std::move(event_graph).finish();
    auto event_plan = graph_jit::detail::build_connection_analysis_plan(
        event_configured, 64);
    ASSERT_FALSE(event_plan.has_value());
    EXPECT_NE(
        event_plan.error().find("breaks an acyclic dependency"),
        std::string::npos);
}

TEST(GraphJitConnectionPlan, RejectsIndexedEdgesInWholeSemanticScc)
{
    using namespace iv;
    GraphBuilder sample_graph;
    auto sample_first =
        details::configure_concrete_node<IndexedSamplePass>(sample_graph);
    auto sample_second =
        details::configure_concrete_node<IndexedSamplePass>(sample_graph);
    sample_first(sample_second);
    sample_second(static_cast<SamplePortRef>(sample_first).detach(5));
    sample_graph.outputs();

    auto sample_configured = std::move(sample_graph).finish();
    auto sample_plan = graph_jit::detail::build_connection_analysis_plan(
        sample_configured, 64);
    ASSERT_FALSE(sample_plan.has_value());
    EXPECT_NE(sample_plan.error().find("indexed sample connection"),
        std::string::npos);
    EXPECT_NE(sample_plan.error().find("semantic SCC"), std::string::npos);

    GraphBuilder event_graph;
    auto event_first =
        details::configure_concrete_node<IndexedEventPass>(event_graph);
    auto event_second =
        details::configure_concrete_node<IndexedEventPass>(event_graph);
    event_first.connect_event_input(0, event_second.event_port());
    event_second.connect_event_input(0, event_first.event_port().detach(7));
    event_graph.outputs();

    auto event_configured = std::move(event_graph).finish();
    auto event_plan = graph_jit::detail::build_connection_analysis_plan(
        event_configured, 64);
    ASSERT_FALSE(event_plan.has_value());
    EXPECT_NE(event_plan.error().find("indexed event connection"),
        std::string::npos);
    EXPECT_NE(event_plan.error().find("semantic SCC"), std::string::npos);
}

TEST(GraphJitConnectionPlan, RejectsIndexedSemanticSelfLoop)
{
    using namespace iv;
    GraphBuilder graph;
    auto node = details::configure_concrete_node<IndexedSamplePass>(graph);
    node(static_cast<SamplePortRef>(node).detach(3));
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_FALSE(plan.has_value());
    EXPECT_NE(plan.error().find("indexed sample connection"),
        std::string::npos);
    EXPECT_NE(plan.error().find("semantic SCC"), std::string::npos);
}

TEST(GraphJitConnectionPlan, AllowsRealtimeSccToExportIndexedData)
{
    using namespace iv;
    GraphBuilder graph;
    auto first = details::configure_concrete_node<RealtimeRecorderPass>(graph);
    auto second = details::configure_concrete_node<PlainSamplePass>(graph);
    auto indexed_sink = details::configure_concrete_node<IndexedSink>(graph);
    first(second);
    second(first["realtime"].detach(5));
    indexed_sink(first["recorded"]);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    EXPECT_TRUE(std::ranges::any_of(
        plan->sample_connections,
        [](graph_jit::detail::SampleConnectionPlan const& connection) {
            return connection.access
                == graph_jit::detail::PlannedConnectionAccess::indexed_to_indexed;
        }));
}

TEST(GraphJitConnectionPlan, DerivesSampleDetachExecutionRegion)
{
    using namespace iv;
    GraphBuilder graph;
    auto first = details::configure_concrete_node<PlainSamplePass>(graph);
    auto second = details::configure_concrete_node<NeutralSamplePass>(graph);
    auto const first_handle = first.node_bundle_handle();
    auto const second_handle = second.node_bundle_handle();
    first(second);
    second(static_cast<SamplePortRef>(first).detach(6));
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    auto const detached = std::ranges::find_if(
        plan->sample_connections,
        [](graph_jit::detail::SampleConnectionPlan const& connection) { return connection.detach.has_value(); });
    ASSERT_NE(detached, plan->sample_connections.end());
    EXPECT_EQ(
        std::ranges::count_if(
            plan->sample_connections,
            [](graph_jit::detail::SampleConnectionPlan const& connection) { return connection.detach.has_value(); }),
        1);
    ASSERT_TRUE(detached->detach.has_value());
    EXPECT_EQ(detached->detach->loop_extra_latency, 6u);
    EXPECT_FALSE(detached->detach->initial_value_override.has_value());
    ASSERT_TRUE(detached->detach_initial_value.has_value());
    EXPECT_FLOAT_EQ(static_cast<float>(*detached->detach_initial_value), 0.375f);
    ASSERT_TRUE(detached->detach_region.has_value());
    ASSERT_LT(*detached->detach_region, plan->schedule.regions.size());
    auto const& region = plan->schedule.regions[*detached->detach_region];
    EXPECT_TRUE(region.cyclic);
    EXPECT_EQ(region.maximum_block_size, 4u);
    EXPECT_EQ(region.scc_feedback_latency, 4u);
    ASSERT_EQ(region.nodes.size(), 2u);
    EXPECT_TRUE(std::ranges::contains(region.nodes, first_handle));
    EXPECT_TRUE(std::ranges::contains(region.nodes, second_handle));

    auto const position = [&](NodeBundleHandle bundle) {
        auto const found = std::ranges::find(region.execution_order, bundle);
        EXPECT_NE(found, region.execution_order.end());
        return static_cast<std::size_t>(
            std::distance(region.execution_order.begin(), found));
    };
    EXPECT_LT(position(second_handle), position(first_handle));
}

TEST(GraphJitConnectionPlan, SampleDetachInitialValueOverrideWins)
{
    using namespace iv;
    GraphBuilder graph;
    auto first = details::configure_concrete_node<PlainSamplePass>(graph);
    auto second = details::configure_concrete_node<NeutralSamplePass>(graph);
    first(second);
    second(static_cast<SamplePortRef>(first).detach(6, Sample{-0.625f}));
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    auto const detached = std::ranges::find_if(
        plan->sample_connections,
        [](graph_jit::detail::SampleConnectionPlan const& connection) { return connection.detach.has_value(); });
    ASSERT_NE(detached, plan->sample_connections.end());
    ASSERT_TRUE(detached->detach.has_value());
    ASSERT_TRUE(detached->detach->initial_value_override.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(*detached->detach->initial_value_override), -0.625f);
    ASSERT_TRUE(detached->detach_initial_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(*detached->detach_initial_value), -0.625f);
}

TEST(GraphJitConnectionPlan, DerivesEventDetachExecutionRegion)
{
    using namespace iv;
    GraphBuilder graph;
    auto first = details::configure_concrete_node<PlainEventPass>(graph);
    auto second = details::configure_concrete_node<PlainEventPass>(graph);
    auto const first_handle = first.node_bundle_handle();
    auto const second_handle = second.node_bundle_handle();
    first.connect_event_input(0, second.event_port());
    second.connect_event_input(0, first.event_port().detach(10));
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    auto const detached = std::ranges::find_if(
        plan->event_connections,
        [](graph_jit::detail::EventConnectionPlan const& connection) { return connection.detach.has_value(); });
    ASSERT_NE(detached, plan->event_connections.end());
    EXPECT_EQ(
        std::ranges::count_if(
            plan->event_connections,
            [](graph_jit::detail::EventConnectionPlan const& connection) { return connection.detach.has_value(); }),
        1);
    EXPECT_EQ(detached->source_type, EventTypeId::trigger);
    ASSERT_TRUE(detached->detach.has_value());
    EXPECT_EQ(detached->detach->loop_extra_latency, 10u);
    ASSERT_TRUE(detached->detach_region.has_value());
    ASSERT_LT(*detached->detach_region, plan->schedule.regions.size());
    auto const& region = plan->schedule.regions[*detached->detach_region];
    EXPECT_TRUE(region.cyclic);
    EXPECT_EQ(region.maximum_block_size, 8u);
    EXPECT_EQ(region.scc_feedback_latency, 8u);
    ASSERT_EQ(region.nodes.size(), 2u);
    EXPECT_TRUE(std::ranges::contains(region.nodes, first_handle));
    EXPECT_TRUE(std::ranges::contains(region.nodes, second_handle));

    auto const position = [&](NodeBundleHandle bundle) {
        auto const found = std::ranges::find(region.execution_order, bundle);
        EXPECT_NE(found, region.execution_order.end());
        return static_cast<std::size_t>(
            std::distance(region.execution_order.begin(), found));
    };
    EXPECT_LT(position(second_handle), position(first_handle));
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
    EXPECT_EQ(group.storage_requirements.retained_frames, 0u);
    ASSERT_TRUE(group.storage_plan.has_value());
    EXPECT_EQ(
        group.storage_plan->kind,
        RealtimeBufferStorageKind::transient_stack);
    ASSERT_EQ(plan->storage.regions.size(), 1u);
    EXPECT_EQ(
        plan->storage.regions.front().lifetime,
        graph_jit::detail::ConnectionStorageLifetime::transient);
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
    EXPECT_EQ(group.storage_requirements.retained_frames, 0u);
    ASSERT_TRUE(group.storage_plan.has_value());
    EXPECT_EQ(
        group.storage_plan->kind,
        RealtimeBufferStorageKind::transient_stack);

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
    EXPECT_FALSE(group.storage_plan.has_value());
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
    ASSERT_TRUE(group.storage_plan.has_value());
    EXPECT_EQ(
        group.storage_plan->kind,
        RealtimeBufferStorageKind::transient_stack);
}

TEST(GraphJitConnectionPlan, IndexedConnectionsDoNotUseRealtimeStoragePolicy)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<IndexedSource>(graph);
    auto sink = details::configure_concrete_node<IndexedSink>(graph);
    sink(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    ASSERT_EQ(plan->sample_connections.size(), 1u);
    EXPECT_EQ(
        plan->sample_connections[0].access,
        graph_jit::detail::PlannedConnectionAccess::indexed_to_indexed);
    ASSERT_EQ(plan->dependencies.size(), 1u);
    EXPECT_FALSE(plan->dependencies[0].sequential_tick_dependency);
    ASSERT_EQ(plan->sample_producer_groups.size(), 1u);
    EXPECT_FALSE(plan->sample_producer_groups[0].has_realtime_connections);
    EXPECT_TRUE(plan->sample_producer_groups[0].has_indexed_connections);
    EXPECT_FALSE(plan->sample_producer_groups[0].storage_plan.has_value());
    EXPECT_TRUE(plan->storage.regions.empty());
}

TEST(GraphJitConnectionPlan, PreservesIndexedToRealtimeAccessDirection)
{
    using namespace iv;

    GraphBuilder indexed_to_realtime;
    auto indexed_source =
        details::configure_concrete_node<IndexedSource>(indexed_to_realtime);
    auto realtime_sink =
        details::configure_concrete_node<RealtimeSink>(indexed_to_realtime);
    realtime_sink(indexed_source);
    indexed_to_realtime.outputs();

    auto indexed_to_realtime_graph = std::move(indexed_to_realtime).finish();
    auto indexed_to_realtime_plan =
        graph_jit::detail::build_connection_analysis_plan(
            indexed_to_realtime_graph, 64);
    ASSERT_TRUE(indexed_to_realtime_plan.has_value())
        << (indexed_to_realtime_plan
                ? std::string{}
                : indexed_to_realtime_plan.error());
    ASSERT_EQ(indexed_to_realtime_plan->sample_connections.size(), 1u);
    EXPECT_EQ(
        indexed_to_realtime_plan->sample_connections[0].access,
        graph_jit::detail::PlannedConnectionAccess::indexed_to_realtime);
    ASSERT_EQ(indexed_to_realtime_plan->dependencies.size(), 1u);
    EXPECT_FALSE(
        indexed_to_realtime_plan->dependencies[0].sequential_tick_dependency);
    ASSERT_EQ(indexed_to_realtime_plan->sample_producer_groups.size(), 1u);
    EXPECT_FALSE(
        indexed_to_realtime_plan->sample_producer_groups[0]
            .has_realtime_connections);
    EXPECT_TRUE(
        indexed_to_realtime_plan->sample_producer_groups[0]
            .has_indexed_connections);
    EXPECT_FALSE(
        indexed_to_realtime_plan->sample_producer_groups[0]
            .storage_plan.has_value());
}

TEST(GraphJitConnectionPlan, RejectsRealtimeToIndexedConnections)
{
    using namespace iv;

    GraphBuilder realtime_to_indexed_sample;
    auto realtime_source =
        details::configure_concrete_node<MonoSource>(realtime_to_indexed_sample);
    auto indexed_sink =
        details::configure_concrete_node<IndexedSink>(realtime_to_indexed_sample);
    indexed_sink(realtime_source);
    realtime_to_indexed_sample.outputs();

    auto realtime_to_indexed_sample_graph =
        std::move(realtime_to_indexed_sample).finish();
    auto realtime_to_indexed_sample_plan =
        graph_jit::detail::build_connection_analysis_plan(
            realtime_to_indexed_sample_graph, 64);
    ASSERT_FALSE(realtime_to_indexed_sample_plan.has_value());
    EXPECT_NE(realtime_to_indexed_sample_plan.error().find("sample connection 0"),
        std::string::npos);
    EXPECT_NE(realtime_to_indexed_sample_plan.error().find(
        "realtime output to an indexed input"), std::string::npos);
    EXPECT_NE(realtime_to_indexed_sample_plan.error().find("tick_record"),
        std::string::npos);

    GraphBuilder realtime_to_indexed_event;
    auto realtime_event_source =
        details::configure_concrete_node<PlainEventPass>(
            realtime_to_indexed_event);
    auto indexed_event_sink =
        details::configure_concrete_node<IndexedEventPass>(
            realtime_to_indexed_event);
    indexed_event_sink.connect_event_input(
        0, realtime_event_source.event_port());
    realtime_to_indexed_event.outputs();

    auto realtime_to_indexed_event_graph =
        std::move(realtime_to_indexed_event).finish();
    auto realtime_to_indexed_event_plan =
        graph_jit::detail::build_connection_analysis_plan(
            realtime_to_indexed_event_graph, 64);
    ASSERT_FALSE(realtime_to_indexed_event_plan.has_value());
    EXPECT_NE(realtime_to_indexed_event_plan.error().find("event connection 0"),
        std::string::npos);
    EXPECT_NE(realtime_to_indexed_event_plan.error().find(
        "realtime output to an indexed input"), std::string::npos);
    EXPECT_NE(realtime_to_indexed_event_plan.error().find("tick_record"),
        std::string::npos);
}
