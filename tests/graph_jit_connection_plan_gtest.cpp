#include <intravenous/dsl.h>
#include <intravenous/graph_jit/connection_plan.h>
#include <intravenous/graph_jit/sample_physical_plan.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TimedSamplePass {
    static constexpr auto inputs()
    {
        return std::array{
            iv::sequential_sample_input(
                "in", {}, iv::SequentialInputConfig{.history = 5}),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::tick_sample_output(
                "out", {}, iv::TickOutputConfig{.history = 3, .latency = 2}),
        };
    }

    void tick_block(iv::TickBlockContext<TimedSamplePass> const&) const {}
};

struct PlainSamplePass {
    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<PlainSamplePass> const&) const {}
};

struct MixedAccessPass {
    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::tick_sample_output("realtime"),
            iv::tock_sample_output(
                "indexed", {}, iv::OutputRetention::persisted),
        };
    }

    void tick_block(iv::TickBlockContext<MixedAccessPass> const&) const {}
    void tock_coverage(iv::TockCoverageContext<MixedAccessPass>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<MixedAccessPass>& context) const
    {
        context.template output<"indexed">().publish_coverage({});
    }
};

struct NeutralSamplePass {
    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input(
            "in", iv::SampleInputProperties{.neutral_value = 0.375f})};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<NeutralSamplePass> const&) const {}
};

struct PlainEventPass {
    static constexpr auto inputs()
    {
        return std::array{
            iv::sequential_event_input("in", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_event_output(
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
        return std::array{iv::tick_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<MonoSource> const&) const {}
};

struct PersistedStereoSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "out",
            iv::SampleOutputProperties{
                .channel_layout = iv::ChannelLayout{
                    .channel_type = iv::ChannelTypeId::stereo,
                    .sample_layout = iv::SampleStreamLayout::planar,
                },
            },
            {},
            iv::OutputRetention::persisted)};
    }

    void tick_block(iv::TickBlockContext<PersistedStereoSource> const&) const {}
};

struct LimitedMonoSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("out")};
    }

    constexpr std::size_t max_block_size() const { return 16; }

    void tick_block(iv::TickBlockContext<LimitedMonoSource> const&) const {}
};

struct StereoSink {
    static constexpr auto inputs()
    {
        return std::array{
            iv::sequential_sample_input(
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
        return std::array{iv::tock_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<IndexedSource> const&) const {}
    void tock_coverage(iv::TockCoverageContext<IndexedSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<IndexedSource>& context) const
    {
        context.template output<"out">().publish_coverage({});
    }
};

struct IndexedStereoSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tock_sample_output(
            "out",
            iv::SampleOutputProperties{
                .channel_layout = iv::ChannelLayout{
                    .channel_type = iv::ChannelTypeId::stereo,
                    .sample_layout = iv::SampleStreamLayout::planar,
                },
            })};
    }

    void tick_block(iv::TickBlockContext<IndexedStereoSource> const&) const {}
    void tock_coverage(iv::TockCoverageContext<IndexedStereoSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<IndexedStereoSource>& context) const
    {
        context.template output<"out">().publish_coverage({});
    }
};

struct IndexedSamplePass {
    static constexpr auto inputs()
    {
        return std::array{iv::random_access_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tock_sample_output("out")};
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
            iv::random_access_event_input("in", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::tock_event_output("out", iv::EventTypeId::trigger),
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

struct IndexedMidiSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::tock_event_output("out", iv::EventTypeId::midi),
        };
    }

    void tick_block(iv::TickBlockContext<IndexedMidiSource> const&) const {}
    void tock_coverage(iv::TockCoverageContext<IndexedMidiSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<IndexedMidiSource>&) const {}
};

struct TockTriggerSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tock_event_output(
            "out",
            iv::EventOutputProperties{
                .type = iv::EventTypeId::trigger,
                .max_events_per_index = 0.5,
            })};
    }

    void tick_block(iv::TickBlockContext<TockTriggerSource> const&) const {}
    void tock_coverage(iv::TockCoverageContext<TockTriggerSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<TockTriggerSource>&) const {}
};

struct IndexedSink {
    static constexpr auto inputs()
    {
        return std::array{iv::random_access_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<IndexedSink> const&) const {}
};

struct IndexedTwoInputPass {
    static constexpr auto inputs()
    {
        return std::array{
            iv::random_access_sample_input("a"),
            iv::random_access_sample_input("b"),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tock_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<IndexedTwoInputPass> const&) const {}
    void tock_coverage(iv::TockCoverageContext<IndexedTwoInputPass>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<IndexedTwoInputPass>&) const {}
};

struct OutputAccessRetentionModes {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::tick_sample_output(
                "realtime_ephemeral", {}, {},
                iv::OutputRetention::ephemeral),
            iv::tick_sample_output(
                "realtime_persisted", {}, {},
                iv::OutputRetention::persisted),
            iv::tock_sample_output(
                "indexed_ephemeral", {},
                iv::OutputRetention::ephemeral),
            iv::tock_sample_output(
                "indexed_persisted", {},
                iv::OutputRetention::persisted),
            iv::tock_event_output(
                "indexed_persisted_events",
                iv::EventOutputProperties{
                    .type = iv::EventTypeId::trigger,
                    .max_events_per_index = 0.25,
                },
                iv::OutputRetention::persisted),
        };
    }

    void tick_block(
        iv::TickBlockContext<OutputAccessRetentionModes> const&) const {}
    void tock_coverage(
        iv::TockCoverageContext<OutputAccessRetentionModes>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<OutputAccessRetentionModes>&) const {}
};

struct RealtimeSink {
    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<RealtimeSink> const&) const {}
};

struct ReplayableSource {
    static constexpr bool intrinsically_replayable = true;

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("out")};
    }

    void tick(iv::TickSampleContext<ReplayableSource> const&) const {}
};

struct ReplayablePass {
    static constexpr bool intrinsically_replayable = true;

    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("out")};
    }

    void tick(iv::TickSampleContext<ReplayablePass> const&) const {}
};

struct PersistedTickPass {
    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "out", {}, {}, iv::OutputRetention::persisted)};
    }

    void tick_block(iv::TickBlockContext<PersistedTickPass> const&) const {}
};

struct LatentSamplePass {
    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "out", {}, iv::TickOutputConfig{.latency = 2})};
    }

    constexpr std::size_t internal_latency() const { return 5; }

    void tick_block(iv::TickBlockContext<LatentSamplePass> const&) const {}
};

struct TwoInputSink {
    static constexpr auto inputs()
    {
        return std::array{
            iv::sequential_sample_input("fast"),
            iv::sequential_sample_input("slow"),
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
            iv::sequential_sample_input("fast"),
            iv::sequential_sample_input("slow"),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "out", {}, iv::TickOutputConfig{.latency = 4})};
    }

    constexpr std::size_t internal_latency() const { return 3; }

    void tick_block(iv::TickBlockContext<LatentTwoInputPass> const&) const {}
};

struct MediumLatencySamplePass {
    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "out", {}, iv::TickOutputConfig{.latency = 1})};
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
        internal->destination_access,
        graph_jit::PlannedDestinationAccess::sequential);
    ASSERT_FALSE(internal->source_channel_timings.empty());
    EXPECT_TRUE(std::ranges::all_of(
        internal->source_channel_timings,
        [](graph_jit::detail::SampleSourceChannelTimingPlan const& source) {
            return source.delivery
                == graph_jit::PlannedDeliveryMechanism::tick_to_sequential;
        }));
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
    auto first = details::configure_concrete_node<MixedAccessPass>(graph);
    auto second = details::configure_concrete_node<PlainSamplePass>(graph);
    auto indexed_sink = details::configure_concrete_node<IndexedSink>(graph);
    auto const first_handle = first.node_bundle_handle();
    auto const second_handle = second.node_bundle_handle();
    auto const indexed_sink_handle = indexed_sink.node_bundle_handle();
    first(second);
    second(first["realtime"].detach(5));
    indexed_sink(first["indexed"]);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    EXPECT_TRUE(std::ranges::any_of(
        plan->sample_connections,
        [](graph_jit::detail::SampleConnectionPlan const& connection) {
            return std::ranges::any_of(
                connection.source_channel_timings,
                [](graph_jit::detail::SampleSourceChannelTimingPlan const& source) {
                    return source.delivery
                        == graph_jit::PlannedDeliveryMechanism::tock_to_random_access;
                });
        }));
    ASSERT_TRUE(plan->indexed.bundle_to_semantic_node[first_handle]);
    ASSERT_TRUE(plan->indexed.bundle_to_semantic_node[second_handle]);
    ASSERT_TRUE(plan->indexed.bundle_to_semantic_node[indexed_sink_handle]);
    auto const first_semantic =
        *plan->indexed.bundle_to_semantic_node[first_handle];
    auto const second_semantic =
        *plan->indexed.bundle_to_semantic_node[second_handle];
    auto const sink_semantic =
        *plan->indexed.bundle_to_semantic_node[indexed_sink_handle];
    EXPECT_EQ(
        plan->indexed.semantic_nodes[first_semantic].scc,
        plan->indexed.semantic_nodes[second_semantic].scc);
    EXPECT_NE(
        plan->indexed.semantic_nodes[first_semantic].scc,
        plan->indexed.semantic_nodes[sink_semantic].scc);
    EXPECT_LT(
        plan->indexed.semantic_nodes[first_semantic].scc,
        plan->indexed.semantic_nodes[sink_semantic].scc);
    EXPECT_TRUE(plan->indexed.semantic_sccs[
        plan->indexed.semantic_nodes[first_semantic].scc].cyclic);
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

TEST(GraphJitConnectionPlan, PartitionsOverlappingSamplePortUsesIntoEndpointAtoms)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<PersistedStereoSource>(graph);
    auto sequential = details::configure_concrete_node<RealtimeSink>(graph);
    auto random_access = details::configure_concrete_node<IndexedSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    sequential(source[stereo::left]);
    random_access(source[stereo::right]);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    auto const& atoms = plan->indexed.sample_source_atoms;
    ASSERT_EQ(atoms.size(), 2u);
    auto const atom_for_channel = [&](std::size_t channel)
        -> graph_jit::SampleSourceEndpointAtomPlan const* {
        auto const found = std::ranges::find_if(
            atoms,
            [&](graph_jit::SampleSourceEndpointAtomPlan const& atom) {
                return atom.port.node_bundle_handle == source_handle
                    && atom.channels.size() == 1
                    && atom.channels.front().channel == channel;
            });
        return found == atoms.end() ? nullptr : &*found;
    };
    auto const* left = atom_for_channel(stereo::left.channel_ordinal);
    auto const* right = atom_for_channel(stereo::right.channel_ordinal);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_TRUE(left->capabilities.current_tick_readable);
    EXPECT_FALSE(right->capabilities.current_tick_readable);
    EXPECT_TRUE(left->capabilities.capture_backed_persistence);
    EXPECT_TRUE(right->capabilities.capture_backed_persistence);
    EXPECT_TRUE(left->capabilities.canonical_persisted_pages);
    EXPECT_TRUE(right->capabilities.canonical_persisted_pages);
    EXPECT_EQ(left->connection_indices.size(), 1u);
    EXPECT_EQ(right->connection_indices.size(), 1u);
    EXPECT_NE(left->connection_indices, right->connection_indices);

    ASSERT_EQ(plan->sample_producer_groups.size(), 1u);
    EXPECT_EQ(plan->sample_producer_groups.front().source_atom_indices.size(), 2u);
    ASSERT_EQ(plan->indexed.connections.size(), 1u);
    ASSERT_EQ(plan->indexed.connections.front().source_atoms.size(), 1u);
    EXPECT_EQ(
        plan->indexed.connections.front().source_atoms.front(),
        static_cast<graph_jit::EndpointAtomOrdinal>(
            right - atoms.data()));

    auto const& physical = plan->indexed.physical;
    ASSERT_EQ(physical.sample_source_representations.size(), atoms.size());
    auto const left_ordinal = static_cast<graph_jit::EndpointAtomOrdinal>(
        left - atoms.data());
    auto const right_ordinal = static_cast<graph_jit::EndpointAtomOrdinal>(
        right - atoms.data());
    auto canonical_for = [&](graph_jit::EndpointAtomOrdinal atom) {
        return std::ranges::find_if(
            physical.sample_source_representations[atom],
            [&](graph_jit::IndexedRepresentationOrdinal representation) {
                return physical.representations[representation].residence
                    == graph_jit::IndexedRepresentationResidence::
                        canonical_persisted_pages;
            });
    };
    auto const left_canonical = canonical_for(left_ordinal);
    auto const right_canonical = canonical_for(right_ordinal);
    ASSERT_NE(
        left_canonical,
        physical.sample_source_representations[left_ordinal].end());
    ASSERT_NE(
        right_canonical,
        physical.sample_source_representations[right_ordinal].end());
    EXPECT_EQ(*left_canonical, *right_canonical);
    EXPECT_EQ(
        physical.representations[*left_canonical].sample_channels,
        (std::vector<std::size_t>{0, 1}));
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
    ASSERT_EQ(plan->event_connections[0].deliveries.size(), 1u);
    EXPECT_EQ(
        plan->event_connections[0].deliveries[0].mechanism,
        graph_jit::PlannedDeliveryMechanism::tick_to_sequential);
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
    ASSERT_EQ(plan->sample_connections[0].source_channel_timings.size(), 1u);
    EXPECT_EQ(
        plan->sample_connections[0].source_channel_timings[0].delivery,
        graph_jit::PlannedDeliveryMechanism::tock_to_random_access);
    ASSERT_EQ(plan->dependencies.size(), 1u);
    EXPECT_FALSE(plan->dependencies[0].sequential_tick_dependency);
    ASSERT_EQ(plan->sample_producer_groups.size(), 1u);
    EXPECT_FALSE(plan->sample_producer_groups[0].has_realtime_connections);
    EXPECT_TRUE(plan->sample_producer_groups[0].has_background_connections);
    EXPECT_FALSE(plan->sample_producer_groups[0].storage_plan.has_value());
    EXPECT_TRUE(plan->storage.regions.empty());
    ASSERT_EQ(plan->indexed.sample_source_atoms.size(), 1u);
    EXPECT_TRUE(plan->indexed.sample_source_atoms.front()
        .capabilities.prepared_addressable_window);
    EXPECT_TRUE(plan->indexed.sample_source_atoms.front()
        .capabilities.transaction_local_addressable);
    ASSERT_EQ(plan->indexed.requestable_outputs.size(), 1u);
    EXPECT_FALSE(plan->indexed.endpoints[
        plan->indexed.requestable_outputs.front()].stable_identity);
}

TEST(GraphJitConnectionPlan, RetainsCompleteIndexedTopologyAndRetention)
{
    using namespace iv;
    GraphBuilder graph;
    auto source =
        details::configure_concrete_node<OutputAccessRetentionModes>(graph);
    auto left = details::configure_concrete_node<IndexedSamplePass>(graph);
    auto right = details::configure_concrete_node<IndexedSamplePass>(graph);
    auto join = details::configure_concrete_node<IndexedTwoInputPass>(graph);
    auto const source_handle = source.node_bundle_handle();
    _annotate_node_source_info(source.node_ref(), "source");
    _annotate_node_source_info(left.node_ref(), "left");
    _annotate_node_source_info(right.node_ref(), "right");
    _annotate_node_source_info(join.node_ref(), "join");

    left(source["indexed_ephemeral"]);
    right(source["indexed_ephemeral"]);
    join("a"_P = left, "b"_P = right);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto built = graph_jit::detail::build_connection_analysis_plan(
        configured, 64);
    ASSERT_TRUE(built.has_value()) << (built ? std::string{} : built.error());
    auto const& plan = built->indexed;

    ASSERT_EQ(plan.nodes.size(), 4u);
    ASSERT_EQ(plan.connections.size(), 4u);
    ASSERT_EQ(plan.components.size(), 1u);
    EXPECT_EQ(plan.component_order, (std::vector<std::size_t>{0}));
    auto const& component = plan.components.front();
    ASSERT_EQ(component.forward_order.size(), 4u);
    EXPECT_EQ(component.tock_order, component.forward_order);
    auto expected_reverse = component.forward_order;
    std::ranges::reverse(expected_reverse);
    EXPECT_EQ(component.reverse_order, expected_reverse);
    EXPECT_EQ(component.connections.size(), 4u);

    EXPECT_EQ(plan.accumulators.input_change_count, 4u);
    EXPECT_EQ(plan.accumulators.input_requirement_count, 4u);
    EXPECT_EQ(plan.accumulators.output_change_count, 7u);
    EXPECT_EQ(plan.accumulators.output_requirement_count, 7u);
    EXPECT_EQ(plan.requestable_outputs.size(), 7u);
    EXPECT_TRUE(std::ranges::all_of(
        plan.endpoints,
        [](graph_jit::IndexedEndpointPlan const& endpoint) {
            return endpoint.retention.has_value()
                == (endpoint.direction
                    == graph_jit::IndexedEndpointDirection::output);
        }));

    auto endpoint_named = [&](std::string_view name)
        -> graph_jit::IndexedEndpointPlan const* {
        auto const found = std::ranges::find_if(
            plan.endpoints,
            [&](graph_jit::IndexedEndpointPlan const& endpoint) {
                return endpoint.name == name;
            });
        return found == plan.endpoints.end() ? nullptr : &*found;
    };
    auto const* indexed_ephemeral = endpoint_named("indexed_ephemeral");
    auto const* indexed_persisted = endpoint_named("indexed_persisted");
    auto const* indexed_persisted_events =
        endpoint_named("indexed_persisted_events");
    auto const* realtime_persisted = endpoint_named("realtime_persisted");
    EXPECT_EQ(endpoint_named("realtime_ephemeral"), nullptr);
    ASSERT_NE(realtime_persisted, nullptr);
    EXPECT_TRUE(realtime_persisted->persisted_tick_output);
    ASSERT_TRUE(realtime_persisted->retention.has_value());
    EXPECT_EQ(*realtime_persisted->retention, OutputRetention::persisted);
    ASSERT_NE(indexed_ephemeral, nullptr);
    ASSERT_NE(indexed_persisted, nullptr);
    ASSERT_NE(indexed_persisted_events, nullptr);
    ASSERT_TRUE(indexed_ephemeral->retention.has_value());
    ASSERT_TRUE(indexed_persisted->retention.has_value());
    ASSERT_TRUE(indexed_persisted_events->retention.has_value());
    EXPECT_EQ(
        *indexed_ephemeral->retention, OutputRetention::ephemeral);
    EXPECT_EQ(
        *indexed_persisted->retention, OutputRetention::persisted);
    EXPECT_EQ(
        *indexed_persisted_events->retention, OutputRetention::persisted);
    ASSERT_TRUE(indexed_ephemeral->stable_identity);
    EXPECT_EQ(indexed_ephemeral->stable_identity->node.graph, "root");
    EXPECT_TRUE(indexed_ephemeral->stable_identity->node.virtual_node
        .starts_with("source#type:"));
    EXPECT_EQ(
        indexed_ephemeral->stable_identity->port_name, "indexed_ephemeral");
    EXPECT_EQ(indexed_ephemeral->outgoing_connections.size(), 2u);

    auto sample_atom_for_port = [&](std::size_t port)
        -> graph_jit::SampleSourceEndpointAtomPlan const* {
        auto const found = std::ranges::find_if(
            plan.sample_source_atoms,
            [&](graph_jit::SampleSourceEndpointAtomPlan const& atom) {
                return atom.port.node_bundle_handle == source_handle
                    && atom.port.port_ordinal == port;
            });
        return found == plan.sample_source_atoms.end() ? nullptr : &*found;
    };
    auto const* tick_ephemeral_atom = sample_atom_for_port(0);
    auto const* tick_persisted_atom = sample_atom_for_port(1);
    auto const* tock_ephemeral_atom = sample_atom_for_port(2);
    auto const* tock_persisted_atom = sample_atom_for_port(3);
    ASSERT_NE(tick_ephemeral_atom, nullptr);
    ASSERT_NE(tick_persisted_atom, nullptr);
    ASSERT_NE(tock_ephemeral_atom, nullptr);
    ASSERT_NE(tock_persisted_atom, nullptr);
    EXPECT_TRUE(tick_ephemeral_atom->connection_indices.empty());
    EXPECT_TRUE(tick_persisted_atom->capabilities.capture_backed_persistence);
    EXPECT_TRUE(tick_persisted_atom->capabilities.canonical_persisted_pages);
    EXPECT_TRUE(tock_ephemeral_atom->capabilities.prepared_addressable_window);
    EXPECT_TRUE(tock_ephemeral_atom->capabilities.transaction_local_addressable);
    EXPECT_TRUE(tock_persisted_atom->connection_indices.empty());
    EXPECT_FALSE(tock_persisted_atom->capabilities.capture_backed_persistence);
    EXPECT_TRUE(tock_persisted_atom->capabilities.canonical_persisted_pages);

    auto const persisted_event_atom = std::ranges::find_if(
        plan.event_source_atoms,
        [&](graph_jit::EventSourceEndpointAtomPlan const& atom) {
            return atom.port.bundle == source_handle && atom.port.port == 0;
        });
    ASSERT_NE(persisted_event_atom, plan.event_source_atoms.end());
    EXPECT_TRUE(persisted_event_atom->connection_indices.empty());
    EXPECT_TRUE(
        persisted_event_atom->capabilities.canonical_persisted_pages);
}

TEST(GraphJitConnectionPlan, RetainsIndexedEventConversion)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<IndexedMidiSource>(graph);
    auto target = details::configure_concrete_node<IndexedEventPass>(graph);
    target.connect_event_input(0, source.event_port());
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto built = graph_jit::detail::build_connection_analysis_plan(
        configured, 64);
    ASSERT_TRUE(built.has_value()) << (built ? std::string{} : built.error());
    ASSERT_EQ(built->indexed.connections.size(), 1u);
    auto const& connection = built->indexed.connections.front();
    EXPECT_EQ(connection.kind, PortKind::event);
    EXPECT_EQ(connection.event_source_type, EventTypeId::midi);
    EXPECT_EQ(connection.event_target_type, EventTypeId::trigger);
    EXPECT_TRUE(connection.requires_conversion);
    EXPECT_GT(connection.event_conversion.size(), 0u);
    ASSERT_EQ(connection.source_endpoints.size(), 1u);
    ASSERT_EQ(connection.target_endpoints.size(), 1u);
    EXPECT_EQ(
        built->indexed.endpoints[connection.source_endpoints.front()]
            .outgoing_connections,
        (std::vector<graph_jit::IndexedConnectionOrdinal>{0}));
    EXPECT_EQ(
        built->indexed.endpoints[connection.target_endpoints.front()]
            .incoming_connections,
        (std::vector<graph_jit::IndexedConnectionOrdinal>{0}));

    auto const& physical = built->indexed.physical;
    ASSERT_EQ(physical.connections.size(), 1u);
    EXPECT_TRUE(physical.connections.front().event_direct_bindings.empty());
    ASSERT_EQ(
        physical.connections.front().event_materializations.size(), 2u);
    std::vector<graph_jit::IndexedRepresentationResidence> residences;
    for (auto const materialization :
         physical.connections.front().event_materializations) {
        ASSERT_LT(materialization, physical.event_materializations.size());
        residences.push_back(
            physical.event_materializations[materialization].residence);
    }
    EXPECT_TRUE(std::ranges::contains(
        residences,
        graph_jit::IndexedRepresentationResidence::prepared_addressable_window));
    EXPECT_TRUE(std::ranges::contains(
        residences,
        graph_jit::IndexedRepresentationResidence::
            transaction_local_addressable));
}

TEST(GraphJitConnectionPlan, SharesEquivalentIndexedEventMaterializations)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<IndexedMidiSource>(graph);
    auto left = details::configure_concrete_node<IndexedEventPass>(graph);
    auto right = details::configure_concrete_node<IndexedEventPass>(graph);
    left.connect_event_input(0, source.event_port());
    right.connect_event_input(0, source.event_port());
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->indexed.connections.size(), 2u);

    auto const& physical = plan->indexed.physical;
    ASSERT_EQ(physical.connections.size(), 2u);
    ASSERT_EQ(physical.event_materializations.size(), 2u);
    ASSERT_TRUE(std::ranges::all_of(
        physical.connections,
        [](graph_jit::IndexedConnectionPhysicalPlan const& connection) {
            return connection.event_materializations.size() == 2;
        }));
    EXPECT_EQ(
        physical.connections[0].event_materializations,
        physical.connections[1].event_materializations);
    for (auto const& materialization : physical.event_materializations) {
        EXPECT_EQ(materialization.connections.size(), 2u);
        EXPECT_EQ(materialization.target_atoms.size(), 2u);
    }
}

TEST(GraphJitConnectionPlan, PreparedAddressableEventMaterializationSubsumesSequential)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<IndexedMidiSource>(graph);
    auto sequential = details::configure_concrete_node<PlainEventPass>(graph);
    auto random_access = details::configure_concrete_node<IndexedEventPass>(graph);
    sequential.connect_event_input(0, source.event_port());
    random_access.connect_event_input(0, source.event_port());
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->indexed.connections.size(), 2u);

    auto const& physical = plan->indexed.physical;
    ASSERT_EQ(physical.event_materializations.size(), 2u);
    ASSERT_EQ(physical.connections[0].event_materializations.size(), 1u);
    ASSERT_EQ(physical.connections[1].event_materializations.size(), 2u);
    auto const prepared = physical.connections[0].event_materializations.front();
    EXPECT_TRUE(std::ranges::contains(
        physical.connections[1].event_materializations, prepared));
    EXPECT_EQ(
        physical.event_materializations[prepared].residence,
        graph_jit::IndexedRepresentationResidence::prepared_addressable_window);
}

TEST(GraphJitConnectionPlan, ExactEventFanoutAliasesPreparedRepresentations)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<TockTriggerSource>(graph);
    auto sequential = details::configure_concrete_node<PlainEventPass>(graph);
    auto random_access = details::configure_concrete_node<IndexedEventPass>(graph);
    sequential.connect_event_input(0, source.event_port());
    random_access.connect_event_input(0, source.event_port());
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->indexed.connections.size(), 2u);

    auto const& physical = plan->indexed.physical;
    EXPECT_TRUE(physical.event_materializations.empty());
    ASSERT_EQ(physical.connections[0].event_direct_bindings.size(), 1u);
    ASSERT_EQ(physical.connections[1].event_direct_bindings.size(), 2u);
    auto const sequential_binding =
        physical.connections[0].event_direct_bindings.front();
    auto const sequential_representation =
        physical.event_direct_bindings[sequential_binding].representation;
    EXPECT_EQ(
        physical.representations[sequential_representation].residence,
        graph_jit::IndexedRepresentationResidence::prepared_addressable_window);
    EXPECT_TRUE(std::ranges::any_of(
        physical.connections[1].event_direct_bindings,
        [&](std::size_t binding) {
            return physical.event_direct_bindings[binding].representation
                == sequential_representation;
        }));
}

TEST(GraphJitConnectionPlan, TockToSequentialUsesPreparedBackgroundDelivery)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<IndexedSource>(graph);
    auto sink = details::configure_concrete_node<NeutralSamplePass>(graph);
    auto const source_handle = source.node_bundle_handle();
    sink(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->sample_connections.size(), 1u);
    ASSERT_EQ(plan->sample_connections[0].source_channel_timings.size(), 1u);
    EXPECT_EQ(
        plan->sample_connections[0].source_channel_timings[0].delivery,
        graph_jit::PlannedDeliveryMechanism::tock_to_sequential);
    ASSERT_EQ(plan->dependencies.size(), 1u);
    EXPECT_FALSE(plan->dependencies[0].sequential_tick_dependency);
    ASSERT_EQ(plan->sample_producer_groups.size(), 1u);
    EXPECT_FALSE(plan->sample_producer_groups[0].has_realtime_connections);
    EXPECT_TRUE(plan->sample_producer_groups[0].has_background_connections);
    EXPECT_FALSE(plan->sample_producer_groups[0].storage_plan.has_value());
    EXPECT_TRUE(plan->storage.regions.empty());

    auto const source_atom = std::ranges::find_if(
        plan->indexed.sample_source_atoms,
        [&](graph_jit::SampleSourceEndpointAtomPlan const& atom) {
            return atom.port.node_bundle_handle == source_handle;
        });
    ASSERT_NE(source_atom, plan->indexed.sample_source_atoms.end());
    EXPECT_TRUE(source_atom->capabilities.prepared_sequential_window);
    EXPECT_FALSE(source_atom->capabilities.prepared_addressable_window);

    ASSERT_EQ(plan->indexed.prepared_sequential_inputs.size(), 1u);
    auto const& prepared = plan->indexed.endpoints[
        plan->indexed.prepared_sequential_inputs.front()];
    EXPECT_TRUE(prepared.prepared_sequential_input);
    EXPECT_FALSE(prepared.random_access_input);
    EXPECT_FLOAT_EQ(static_cast<float>(prepared.sample_neutral_value), 0.375f);
}

TEST(GraphJitConnectionPlan, PreparedAddressableAtomSubsumesSequentialWindow)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<IndexedSource>(graph);
    auto sequential = details::configure_concrete_node<RealtimeSink>(graph);
    auto random_access = details::configure_concrete_node<IndexedSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    sequential(source);
    random_access(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    auto const atom = std::ranges::find_if(
        plan->indexed.sample_source_atoms,
        [&](graph_jit::SampleSourceEndpointAtomPlan const& candidate) {
            return candidate.port.node_bundle_handle == source_handle;
        });
    ASSERT_NE(atom, plan->indexed.sample_source_atoms.end());
    EXPECT_EQ(atom->connection_indices.size(), 2u);
    EXPECT_TRUE(atom->capabilities.prepared_addressable_window);
    EXPECT_TRUE(atom->capabilities.transaction_local_addressable);
    EXPECT_FALSE(atom->capabilities.prepared_sequential_window);

    auto const atom_ordinal = static_cast<graph_jit::EndpointAtomOrdinal>(
        std::distance(plan->indexed.sample_source_atoms.begin(), atom));
    auto const& physical = plan->indexed.physical;
    ASSERT_EQ(physical.sample_source_representations[atom_ordinal].size(), 2u);
    std::vector<graph_jit::IndexedRepresentationResidence> residences;
    for (auto const representation :
         physical.sample_source_representations[atom_ordinal]) {
        residences.push_back(physical.representations[representation].residence);
    }
    EXPECT_TRUE(std::ranges::contains(
        residences,
        graph_jit::IndexedRepresentationResidence::prepared_addressable_window));
    EXPECT_TRUE(std::ranges::contains(
        residences,
        graph_jit::IndexedRepresentationResidence::
            transaction_local_addressable));
    EXPECT_FALSE(std::ranges::contains(
        residences,
        graph_jit::IndexedRepresentationResidence::prepared_sequential_window));
    ASSERT_EQ(physical.connections.size(), 2u);
    EXPECT_TRUE(std::ranges::all_of(
        physical.connections,
        [](graph_jit::IndexedConnectionPhysicalPlan const& connection) {
            return !connection.sample_direct_bindings.empty()
                && connection.sample_materializations.empty();
        }));
}

TEST(GraphJitConnectionPlan, SharesEquivalentIndexedSampleMaterializations)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<IndexedStereoSource>(graph);
    auto left = details::configure_concrete_node<RealtimeSink>(graph);
    auto right = details::configure_concrete_node<RealtimeSink>(graph);
    left(source);
    right(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->indexed.connections.size(), 2u);

    auto const& physical = plan->indexed.physical;
    ASSERT_EQ(physical.connections.size(), 2u);
    ASSERT_EQ(physical.sample_materializations.size(), 1u);
    ASSERT_TRUE(std::ranges::all_of(
        physical.connections,
        [](graph_jit::IndexedConnectionPhysicalPlan const& connection) {
            return connection.sample_direct_bindings.empty()
                && connection.sample_materializations.size() == 1;
        }));
    EXPECT_EQ(
        physical.connections[0].sample_materializations,
        physical.connections[1].sample_materializations);
    auto const& materialization = physical.sample_materializations.front();
    EXPECT_EQ(
        materialization.residence,
        graph_jit::IndexedRepresentationResidence::prepared_sequential_window);
    EXPECT_EQ(materialization.connections.size(), 2u);
    EXPECT_EQ(materialization.target_atoms.size(), 2u);
    EXPECT_EQ(materialization.source_channels.size(), 2u);
}

TEST(GraphJitConnectionPlan, PreparedAddressableSampleMaterializationSubsumesSequential)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<IndexedStereoSource>(graph);
    auto sequential = details::configure_concrete_node<RealtimeSink>(graph);
    auto random_access = details::configure_concrete_node<IndexedSamplePass>(graph);
    sequential(source);
    random_access(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->indexed.connections.size(), 2u);

    auto const& physical = plan->indexed.physical;
    ASSERT_EQ(physical.sample_materializations.size(), 2u);
    ASSERT_EQ(physical.connections[0].sample_materializations.size(), 1u);
    ASSERT_EQ(physical.connections[1].sample_materializations.size(), 2u);
    auto const prepared = physical.connections[0].sample_materializations.front();
    EXPECT_TRUE(std::ranges::contains(
        physical.connections[1].sample_materializations, prepared));
    EXPECT_EQ(
        physical.sample_materializations[prepared].residence,
        graph_jit::IndexedRepresentationResidence::prepared_addressable_window);
}

TEST(GraphJitConnectionPlan, IndexedSampleMaterializationIsTargetAtomLocal)
{
    using namespace iv;

    GraphBuilder graph;
    auto mono = details::configure_concrete_node<MonoSource>(graph);
    auto stereo_source =
        details::configure_concrete_node<IndexedStereoSource>(graph);
    auto sink = details::configure_concrete_node<StereoSink>(graph);
    auto const mono_handle = mono.node_bundle_handle();
    auto const stereo_handle = stereo_source.node_bundle_handle();
    auto const sink_handle = sink.node_bundle_handle();

    // Give the builder an ordinary connection, then replace the lossless
    // configured form with the projection composition we need to exercise:
    // left is a direct Tick mono alias while right is an unrelated Tock
    // stereo -> mono conversion.
    sink(graph.tile<stereo>(mono, mono));
    graph.outputs();
    auto configured = std::move(graph).finish();
    std::array<ConfiguredSampleConnection, 2> connections{
        ConfiguredSampleConnection{
            .source_type = ChannelTypeId::mono,
            .source_channels = {
                SampleOutputChannelId{mono_handle, 0u, 0u},
            },
            .target_type = ChannelTypeId::mono,
            .target_channels = {
                SampleInputChannelId{sink_handle, 0u, 0u},
            },
        },
        ConfiguredSampleConnection{
            .source_type = ChannelTypeId::stereo,
            .source_channels = {
                SampleOutputChannelId{stereo_handle, 0u, 0u},
                SampleOutputChannelId{stereo_handle, 0u, 1u},
            },
            .target_type = ChannelTypeId::mono,
            .target_channels = {
                SampleInputChannelId{sink_handle, 0u, 1u},
            },
        },
    };
    configured.connections = GraphBuilderConnections::from_configured_connections(
        connections,
        std::span<ConfiguredEventConnection const>{});

    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->sample_connections.size(), 1u);
    ASSERT_EQ(plan->sample_connections.front().projection_contributions.size(), 2u);
    ASSERT_EQ(plan->indexed.connections.size(), 1u);

    auto const& physical = plan->indexed.physical;
    ASSERT_EQ(physical.connections.size(), 1u);
    ASSERT_EQ(physical.connections.front().sample_direct_bindings.size(), 1u);
    ASSERT_EQ(physical.connections.front().sample_materializations.size(), 1u);
    ASSERT_EQ(physical.sample_materializations.size(), 1u);

    auto const& direct = physical.sample_direct_bindings[
        physical.connections.front().sample_direct_bindings.front()];
    EXPECT_EQ(direct.target_channel, 0u);
    EXPECT_EQ(
        physical.representations[direct.representation].residence,
        graph_jit::IndexedRepresentationResidence::current_tick);

    auto const& materialization = physical.sample_materializations[
        physical.connections.front().sample_materializations.front()];
    EXPECT_EQ(
        materialization.residence,
        graph_jit::IndexedRepresentationResidence::prepared_sequential_window);
    EXPECT_EQ(materialization.target_channels, (std::vector<std::size_t>{1u}));
    ASSERT_EQ(materialization.source_channels.size(), 2u);
    EXPECT_TRUE(std::ranges::all_of(
        materialization.source_channels,
        [&](SampleOutputChannelId channel) {
            return channel.bundle == stereo_handle;
        }));
    ASSERT_EQ(materialization.projections.size(), 1u);
    EXPECT_EQ(
        materialization.projections.front().source_type,
        ChannelTypeId::stereo);
    EXPECT_EQ(
        materialization.projections.front().target_type,
        ChannelTypeId::mono);
    EXPECT_EQ(
        materialization.projections.front().source_channel_indices,
        (std::vector<std::size_t>{0u, 1u}));
    EXPECT_EQ(
        materialization.projections.front().target_channels,
        (std::vector<std::size_t>{1u}));
}

TEST(GraphJitConnectionPlan, MixedTickAndTockSampleTilePlansPerSourceChannel)
{
    using namespace iv;

    GraphBuilder graph;
    auto tick = details::configure_concrete_node<MonoSource>(graph);
    auto tock = details::configure_concrete_node<IndexedSource>(graph);
    auto sink = details::configure_concrete_node<StereoSink>(graph);
    auto const tick_handle = tick.node_bundle_handle();
    auto const tock_handle = tock.node_bundle_handle();
    auto const sink_handle = sink.node_bundle_handle();
    sink(graph.tile<stereo>(tick, tock));
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->sample_connections.size(), 1u);
    auto const& connection = plan->sample_connections.front();
    ASSERT_EQ(connection.source_channel_timings.size(), 2u);
    EXPECT_EQ(connection.source_channel_timings[0].source.bundle, tick_handle);
    EXPECT_EQ(
        connection.source_channel_timings[0].delivery,
        graph_jit::PlannedDeliveryMechanism::tick_to_sequential);
    EXPECT_EQ(connection.source_channel_timings[1].source.bundle, tock_handle);
    EXPECT_EQ(
        connection.source_channel_timings[1].delivery,
        graph_jit::PlannedDeliveryMechanism::tock_to_sequential);

    std::vector<graph_jit::SampleTargetEndpointAtomPlan const*> target_atoms;
    for (auto const& atom : plan->indexed.sample_target_atoms) {
        if (atom.port.node_bundle_handle == sink_handle) {
            target_atoms.push_back(&atom);
        }
    }
    ASSERT_EQ(target_atoms.size(), 2u);
    EXPECT_TRUE(std::ranges::any_of(
        target_atoms,
        [](graph_jit::SampleTargetEndpointAtomPlan const* atom) {
            return atom->channels.size() == 1
                && atom->capabilities.current_tick_readable;
        }));
    EXPECT_TRUE(std::ranges::any_of(
        target_atoms,
        [](graph_jit::SampleTargetEndpointAtomPlan const* atom) {
            return atom->channels.size() == 1
                && atom->capabilities.prepared_sequential_window;
        }));

    auto const tick_group = std::ranges::find_if(
        plan->sample_producer_groups,
        [&](graph_jit::detail::SampleProducerGroupPlan const& group) {
            return group.source_port
                && group.source_port->node_bundle_handle == tick_handle;
        });
    auto const tock_group = std::ranges::find_if(
        plan->sample_producer_groups,
        [&](graph_jit::detail::SampleProducerGroupPlan const& group) {
            return group.source_port
                && group.source_port->node_bundle_handle == tock_handle;
        });
    ASSERT_NE(tick_group, plan->sample_producer_groups.end());
    ASSERT_NE(tock_group, plan->sample_producer_groups.end());
    EXPECT_TRUE(tick_group->has_realtime_connections);
    EXPECT_FALSE(tick_group->has_background_connections);
    EXPECT_FALSE(tock_group->has_realtime_connections);
    EXPECT_TRUE(tock_group->has_background_connections);

    auto physical = graph_jit::detail::build_sample_physical_plan(*plan, 64);
    ASSERT_TRUE(physical.has_value())
        << (physical ? std::string{} : physical.error());
    auto const tick_group_index = static_cast<std::size_t>(
        std::distance(plan->sample_producer_groups.begin(), tick_group));
    auto const tock_group_index = static_cast<std::size_t>(
        std::distance(plan->sample_producer_groups.begin(), tock_group));
    ASSERT_TRUE(physical->producer_groups[tick_group_index].has_value());
    EXPECT_FALSE(physical->producer_groups[tock_group_index].has_value());
    ASSERT_EQ(physical->connection_representations.size(), 1u);
    EXPECT_FALSE(physical->connection_representations[0].has_value());
}

TEST(GraphJitConnectionPlan, MixedTickAndTockEventFanInPlansPerSource)
{
    using namespace iv;

    GraphBuilder graph;
    auto tick = details::configure_concrete_node<PlainEventPass>(graph);
    auto tock = details::configure_concrete_node<TockTriggerSource>(graph);
    auto sink = details::configure_concrete_node<PlainEventPass>(graph);
    auto const tick_handle = tick.node_bundle_handle();
    auto const tock_handle = tock.node_bundle_handle();
    auto const sink_handle = sink.node_bundle_handle();
    auto tick_port = tick.event_port();
    auto tock_port = tock.event_port();
    std::array<EventOutputPortId, 2> sources{
        tick_port.sources().front(),
        tock_port.sources().front(),
    };
    sink.connect_event_input(
        0, graph.make_event_port(EventTypeId::trigger, sources));
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->event_connections.size(), 1u);
    auto const& connection = plan->event_connections.front();
    ASSERT_EQ(connection.deliveries.size(), 2u);
    EXPECT_TRUE(std::ranges::any_of(
        connection.deliveries,
        [&](graph_jit::detail::EventDeliveryPlan const& delivery) {
            auto const& source = connection.source_plans[delivery.source_index];
            return source.source.bundle == tick_handle
                && delivery.mechanism
                    == graph_jit::PlannedDeliveryMechanism::tick_to_sequential;
        }));
    EXPECT_TRUE(std::ranges::any_of(
        connection.deliveries,
        [&](graph_jit::detail::EventDeliveryPlan const& delivery) {
            auto const& source = connection.source_plans[delivery.source_index];
            return source.source.bundle == tock_handle
                && delivery.mechanism
                    == graph_jit::PlannedDeliveryMechanism::tock_to_sequential;
        }));

    ASSERT_EQ(plan->event_producer_groups.size(), 1u);
    auto const& group = plan->event_producer_groups.front();
    EXPECT_TRUE(group.has_realtime_connections);
    EXPECT_TRUE(group.has_background_connections);
    EXPECT_EQ(group.realtime_sources.size(), 1u);
    EXPECT_EQ(group.background_sources.size(), 1u);
    EXPECT_EQ(group.realtime_sources.front().bundle, tick_handle);
    EXPECT_EQ(group.background_sources.front().bundle, tock_handle);
    EXPECT_DOUBLE_EQ(group.max_events_per_index, 0.25);
    ASSERT_EQ(group.source_atom_indices.size(), 2u);
    ASSERT_EQ(group.target_atom_indices.size(), 1u);
    ASSERT_GE(plan->indexed.event_source_atoms.size(), 2u);
    ASSERT_GE(plan->indexed.event_target_atoms.size(), 2u);
    auto const sink_target_atom = std::ranges::find_if(
        plan->indexed.event_target_atoms,
        [&](graph_jit::EventTargetEndpointAtomPlan const& atom) {
            return atom.port.bundle == sink_handle;
        });
    ASSERT_NE(sink_target_atom, plan->indexed.event_target_atoms.end());
    EXPECT_EQ(sink_target_atom->source_atoms.size(), 2u);
    EXPECT_TRUE(sink_target_atom->capabilities.current_tick_readable);
    EXPECT_TRUE(sink_target_atom->capabilities.prepared_sequential_window);
    auto const disconnected_tick_target_atom = std::ranges::find_if(
        plan->indexed.event_target_atoms,
        [&](graph_jit::EventTargetEndpointAtomPlan const& atom) {
            return atom.port.bundle == tick_handle;
        });
    ASSERT_NE(
        disconnected_tick_target_atom,
        plan->indexed.event_target_atoms.end());
    EXPECT_TRUE(disconnected_tick_target_atom->source_atoms.empty());
    EXPECT_TRUE(disconnected_tick_target_atom->connection_indices.empty());
    auto const tick_atom = std::ranges::find_if(
        plan->indexed.event_source_atoms,
        [&](graph_jit::EventSourceEndpointAtomPlan const& atom) {
            return atom.port.bundle == tick_handle;
        });
    auto const tock_atom = std::ranges::find_if(
        plan->indexed.event_source_atoms,
        [&](graph_jit::EventSourceEndpointAtomPlan const& atom) {
            return atom.port.bundle == tock_handle;
        });
    ASSERT_NE(tick_atom, plan->indexed.event_source_atoms.end());
    ASSERT_NE(tock_atom, plan->indexed.event_source_atoms.end());
    EXPECT_TRUE(tick_atom->capabilities.current_tick_readable);
    EXPECT_TRUE(tock_atom->capabilities.prepared_sequential_window);

    auto const& physical = plan->indexed.physical;
    ASSERT_EQ(physical.connections.size(), 1u);
    EXPECT_TRUE(physical.connections.front().event_direct_bindings.empty());
    ASSERT_EQ(
        physical.connections.front().event_materializations.size(), 1u);
    auto const materialization_ordinal =
        physical.connections.front().event_materializations.front();
    ASSERT_LT(
        materialization_ordinal, physical.event_materializations.size());
    auto const& materialization =
        physical.event_materializations[materialization_ordinal];
    EXPECT_EQ(
        materialization.residence,
        graph_jit::IndexedRepresentationResidence::current_tick);
    ASSERT_EQ(materialization.input_representations.size(), 2u);
    std::vector<graph_jit::IndexedRepresentationResidence> input_residences;
    for (auto const representation : materialization.input_representations) {
        input_residences.push_back(
            physical.representations[representation].residence);
    }
    EXPECT_TRUE(std::ranges::contains(
        input_residences,
        graph_jit::IndexedRepresentationResidence::current_tick));
    EXPECT_TRUE(std::ranges::contains(
        input_residences,
        graph_jit::IndexedRepresentationResidence::prepared_sequential_window));
    EXPECT_DOUBLE_EQ(
        physical.representations[materialization.output_representation]
            .max_events_per_index,
        0.75);
}

TEST(GraphJitConnectionPlan, PersistedTickToRandomAccessUsesStoredBoundary)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<OutputAccessRetentionModes>(graph);
    auto sink = details::configure_concrete_node<IndexedSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    auto const sink_handle = sink.node_bundle_handle();
    sink(source["realtime_persisted"]);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->sample_connections.size(), 1u);
    ASSERT_EQ(plan->sample_connections[0].source_channel_timings.size(), 1u);
    EXPECT_EQ(
        plan->sample_connections[0].source_channel_timings[0].delivery,
        graph_jit::PlannedDeliveryMechanism::persisted_tick_to_random_access);
    ASSERT_TRUE(plan->indexed.bundle_to_indexed_node[source_handle]);
    auto const source_node = *plan->indexed.bundle_to_indexed_node[source_handle];
    auto const endpoint = std::ranges::find_if(
        plan->indexed.endpoints,
        [&](graph_jit::IndexedEndpointPlan const& candidate) {
            return candidate.node == source_node
                && candidate.name == "realtime_persisted";
        });
    ASSERT_NE(endpoint, plan->indexed.endpoints.end());
    EXPECT_TRUE(endpoint->persisted_tick_output);
    EXPECT_FALSE(endpoint->replayed_tick_output);

    auto const source_atom = std::ranges::find_if(
        plan->indexed.sample_source_atoms,
        [&](graph_jit::SampleSourceEndpointAtomPlan const& atom) {
            return atom.port.node_bundle_handle == source_handle
                && atom.port.port_ordinal == 1;
        });
    ASSERT_NE(source_atom, plan->indexed.sample_source_atoms.end());
    EXPECT_TRUE(source_atom->capabilities.capture_backed_persistence);
    EXPECT_TRUE(source_atom->capabilities.canonical_persisted_pages);
    EXPECT_FALSE(source_atom->capabilities.current_tick_readable);
    auto const target_atom = std::ranges::find_if(
        plan->indexed.sample_target_atoms,
        [&](graph_jit::SampleTargetEndpointAtomPlan const& atom) {
            return atom.port.node_bundle_handle == sink_handle;
        });
    ASSERT_NE(target_atom, plan->indexed.sample_target_atoms.end());
    EXPECT_FALSE(target_atom->capabilities.capture_backed_persistence);
    EXPECT_TRUE(target_atom->capabilities.canonical_persisted_pages);
}

TEST(GraphJitConnectionPlan, IntrinsicTickReplaySuppliesRandomAccess)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<ReplayableSource>(graph);
    auto sink = details::configure_concrete_node<IndexedSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    sink(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->sample_connections.size(), 1u);
    auto const& timing = plan->sample_connections[0].source_channel_timings[0];
    EXPECT_EQ(
        timing.delivery,
        graph_jit::PlannedDeliveryMechanism::replayed_tick_to_random_access);
    EXPECT_TRUE(timing.contextually_replayable);
    EXPECT_TRUE(std::ranges::contains(
        plan->indexed.intrinsic_replay_candidates, source_handle));

    ASSERT_TRUE(plan->indexed.bundle_to_indexed_node[source_handle]);
    auto const source_node = *plan->indexed.bundle_to_indexed_node[source_handle];
    EXPECT_TRUE(plan->indexed.nodes[source_node].synthesized_tick_replay);
    EXPECT_TRUE(plan->indexed.nodes[source_node].synthesized_forward_coverage);
    EXPECT_TRUE(plan->indexed.nodes[source_node].synthesized_reverse_coverage);
    EXPECT_TRUE(plan->indexed.nodes[source_node].uses_imported_tick_block_for_replay);
}

TEST(GraphJitConnectionPlan, ContextualReplayTraversesSequentialDependencies)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<ReplayableSource>(graph);
    auto pass = details::configure_concrete_node<ReplayablePass>(graph);
    auto sink = details::configure_concrete_node<IndexedSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    auto const pass_handle = pass.node_bundle_handle();
    pass(source);
    sink(pass);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    auto const source_node = *plan->indexed.bundle_to_indexed_node[source_handle];
    auto const pass_node = *plan->indexed.bundle_to_indexed_node[pass_handle];
    EXPECT_TRUE(plan->indexed.nodes[source_node].synthesized_tick_replay);
    EXPECT_TRUE(plan->indexed.nodes[pass_node].synthesized_tick_replay);
    ASSERT_EQ(plan->indexed.background_evaluation_order.size(), 2u);
    auto const source_position = std::ranges::find(
        plan->indexed.background_evaluation_order, source_node);
    auto const pass_position = std::ranges::find(
        plan->indexed.background_evaluation_order, pass_node);
    ASSERT_NE(source_position, plan->indexed.background_evaluation_order.end());
    ASSERT_NE(pass_position, plan->indexed.background_evaluation_order.end());
    EXPECT_LT(source_position, pass_position);
    EXPECT_TRUE(std::ranges::any_of(
        plan->indexed.background_dependencies,
        [&](graph_jit::IndexedBackgroundDependencyPlan const& dependency) {
            return dependency.source_node == source_node
                && dependency.target_node == pass_node
                && dependency.kind
                    == graph_jit::IndexedBackgroundDependencyKind::replay_sequential;
        }));

    auto const replay_connection = std::ranges::find_if(
        plan->indexed.connections,
        [&](graph_jit::IndexedConnectionPlan const& connection) {
            return connection.kind == PortKind::sample
                && std::ranges::any_of(
                    connection.sample_target_channels,
                    [&](SampleInputChannelId channel) {
                        return channel.bundle == pass_handle;
                    });
        });
    ASSERT_NE(replay_connection, plan->indexed.connections.end());
    auto const replay_connection_ordinal = static_cast<std::size_t>(
        std::distance(plan->indexed.connections.begin(), replay_connection));
    auto const& physical = plan->indexed.physical;
    ASSERT_LT(replay_connection_ordinal, physical.connections.size());
    auto const& physical_connection =
        physical.connections[replay_connection_ordinal];
    ASSERT_EQ(physical_connection.sample_direct_bindings.size(), 2u);
    std::vector<graph_jit::IndexedRepresentationResidence> residences;
    for (auto const binding : physical_connection.sample_direct_bindings) {
        ASSERT_LT(binding, physical.sample_direct_bindings.size());
        residences.push_back(physical.representations[
            physical.sample_direct_bindings[binding].representation].residence);
    }
    EXPECT_TRUE(std::ranges::contains(
        residences,
        graph_jit::IndexedRepresentationResidence::current_tick));
    EXPECT_TRUE(std::ranges::contains(
        residences,
        graph_jit::IndexedRepresentationResidence::
            transaction_local_addressable));
}

TEST(GraphJitConnectionPlan, TockDependencyCanFeedSynthesizedReplay)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<IndexedSource>(graph);
    auto pass = details::configure_concrete_node<ReplayablePass>(graph);
    auto sink = details::configure_concrete_node<IndexedSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    auto const pass_handle = pass.node_bundle_handle();
    pass(source);
    sink(pass);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    auto const source_node = *plan->indexed.bundle_to_indexed_node[source_handle];
    auto const pass_node = *plan->indexed.bundle_to_indexed_node[pass_handle];
    EXPECT_TRUE(plan->indexed.nodes[source_node].authored_tock_execution);
    EXPECT_TRUE(plan->indexed.nodes[pass_node].synthesized_tick_replay);
    auto const source_position = std::ranges::find(
        plan->indexed.background_evaluation_order, source_node);
    auto const pass_position = std::ranges::find(
        plan->indexed.background_evaluation_order, pass_node);
    ASSERT_NE(source_position, plan->indexed.background_evaluation_order.end());
    ASSERT_NE(pass_position, plan->indexed.background_evaluation_order.end());
    EXPECT_LT(source_position, pass_position);

    auto const replay_connection = std::ranges::find_if(
        plan->indexed.connections,
        [&](graph_jit::IndexedConnectionPlan const& connection) {
            return connection.kind == PortKind::sample
                && std::ranges::any_of(
                    connection.sample_target_channels,
                    [&](SampleInputChannelId channel) {
                        return channel.bundle == pass_handle;
                    });
        });
    ASSERT_NE(replay_connection, plan->indexed.connections.end());
    auto const replay_connection_ordinal = static_cast<std::size_t>(
        std::distance(plan->indexed.connections.begin(), replay_connection));
    auto const& physical = plan->indexed.physical;
    auto const& physical_connection =
        physical.connections[replay_connection_ordinal];
    ASSERT_EQ(physical_connection.sample_direct_bindings.size(), 2u);
    std::vector<graph_jit::IndexedRepresentationResidence> residences;
    for (auto const binding : physical_connection.sample_direct_bindings) {
        residences.push_back(physical.representations[
            physical.sample_direct_bindings[binding].representation].residence);
    }
    EXPECT_TRUE(std::ranges::contains(
        residences,
        graph_jit::IndexedRepresentationResidence::prepared_sequential_window));
    EXPECT_TRUE(std::ranges::contains(
        residences,
        graph_jit::IndexedRepresentationResidence::
            transaction_local_addressable));
}

TEST(GraphJitConnectionPlan, PersistedTickBoundaryStopsContextualReplayTraversal)
{
    using namespace iv;

    GraphBuilder graph;
    auto live = details::configure_concrete_node<MonoSource>(graph);
    auto recorder = details::configure_concrete_node<PersistedTickPass>(graph);
    auto replay = details::configure_concrete_node<ReplayablePass>(graph);
    auto sink = details::configure_concrete_node<IndexedSink>(graph);
    auto const recorder_handle = recorder.node_bundle_handle();
    auto const replay_handle = replay.node_bundle_handle();
    recorder(live);
    replay(recorder);
    sink(replay);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    auto const recorder_node = *plan->indexed.bundle_to_indexed_node[recorder_handle];
    auto const replay_node = *plan->indexed.bundle_to_indexed_node[replay_handle];
    EXPECT_FALSE(plan->indexed.nodes[recorder_node].synthesized_tick_replay);
    EXPECT_TRUE(plan->indexed.nodes[replay_node].synthesized_tick_replay);
    EXPECT_TRUE(std::ranges::any_of(
        plan->indexed.background_dependencies,
        [&](graph_jit::IndexedBackgroundDependencyPlan const& dependency) {
            return dependency.source_node == recorder_node
                && dependency.target_node == replay_node
                && dependency.source_is_stored_boundary;
        }));
}

TEST(GraphJitConnectionPlan, RejectsUnreproducibleTickEphemeralRandomAccess)
{
    using namespace iv;

    GraphBuilder sample_graph;
    auto sample_source = details::configure_concrete_node<MonoSource>(sample_graph);
    auto sample_sink = details::configure_concrete_node<IndexedSink>(sample_graph);
    sample_sink(sample_source);
    sample_graph.outputs();
    auto sample_configured = std::move(sample_graph).finish();
    auto sample_plan = graph_jit::detail::build_connection_analysis_plan(
        sample_configured, 64);
    ASSERT_FALSE(sample_plan.has_value());
    EXPECT_NE(sample_plan.error().find("unreproducible Tick/ephemeral"),
        std::string::npos);
    EXPECT_NE(sample_plan.error().find("explicit recorder"), std::string::npos);

    GraphBuilder event_graph;
    auto event_source = details::configure_concrete_node<PlainEventPass>(event_graph);
    auto event_sink = details::configure_concrete_node<IndexedEventPass>(event_graph);
    event_sink.connect_event_input(0, event_source.event_port());
    event_graph.outputs();
    auto event_configured = std::move(event_graph).finish();
    auto event_plan = graph_jit::detail::build_connection_analysis_plan(
        event_configured, 64);
    ASSERT_FALSE(event_plan.has_value());
    EXPECT_NE(event_plan.error().find("unreproducible Tick/ephemeral"),
        std::string::npos);
    EXPECT_NE(event_plan.error().find("explicit recorder"), std::string::npos);
}

TEST(GraphJitConnectionPlan, RejectsUnreproducibleUpstreamDuringTransitiveReplay)
{
    using namespace iv;

    GraphBuilder graph;
    auto live = details::configure_concrete_node<MonoSource>(graph);
    auto replay = details::configure_concrete_node<ReplayablePass>(graph);
    auto sink = details::configure_concrete_node<IndexedSink>(graph);
    replay(live);
    sink(replay);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_FALSE(plan.has_value());
    EXPECT_NE(plan.error().find("unreproducible Tick/ephemeral"),
        std::string::npos);
    EXPECT_NE(plan.error().find("explicit recorder"), std::string::npos);
}

TEST(GraphJitConnectionPlan, RejectsUnavailableBoundarySourceDuringTransitiveReplay)
{
    using namespace iv;

    GraphBuilder graph;
    auto input = graph.input<"in">();
    auto replay = details::configure_concrete_node<ReplayablePass>(graph);
    auto sink = details::configure_concrete_node<IndexedSink>(graph);
    replay(input);
    sink(replay);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_FALSE(plan.has_value());
    EXPECT_NE(plan.error().find("unavailable live sample connection"),
        std::string::npos);
    EXPECT_NE(plan.error().find("explicit recorder"), std::string::npos);
}

TEST(GraphJitConnectionPlan, RejectsContextualReplayCycles)
{
    using namespace iv;

    GraphBuilder graph;
    auto first = details::configure_concrete_node<ReplayablePass>(graph);
    auto second = details::configure_concrete_node<ReplayablePass>(graph);
    auto sink = details::configure_concrete_node<IndexedSink>(graph);
    first(second);
    second(static_cast<SamplePortRef>(first).detach(3));
    sink(first);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_FALSE(plan.has_value());
    EXPECT_NE(plan.error().find("contextual replay dependency cycle"),
        std::string::npos);
}
