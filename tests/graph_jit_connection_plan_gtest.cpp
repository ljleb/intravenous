#include <intravenous/dsl.h>
#include <intravenous/graph_jit/connection_plan.h>
#include <intravenous/graph_jit/sample_storage_plan.h>

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
                "background", {}, iv::OutputRetention::persisted),
        };
    }

    void tick_block(iv::TickBlockContext<MixedAccessPass> const&) const {}
    void tock_coverage(iv::TockCoverageContext<MixedAccessPass>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<MixedAccessPass>& context) const
    {
        context.template output<"background">().publish_coverage({});
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

struct BackgroundSource {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tock_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<BackgroundSource> const&) const {}
    void tock_coverage(iv::TockCoverageContext<BackgroundSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<BackgroundSource>& context) const
    {
        context.template output<"out">().publish_coverage({});
    }
};

struct BackgroundStereoSource {
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

    void tick_block(iv::TickBlockContext<BackgroundStereoSource> const&) const {}
    void tock_coverage(iv::TockCoverageContext<BackgroundStereoSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<BackgroundStereoSource>& context) const
    {
        context.template output<"out">().publish_coverage({});
    }
};

struct BackgroundSamplePass {
    static constexpr auto inputs()
    {
        return std::array{iv::random_access_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::tock_sample_output("out")};
    }

    void tick_block(iv::TickBlockContext<BackgroundSamplePass> const&) const {}
    void tock_coverage(iv::TockCoverageContext<BackgroundSamplePass>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<BackgroundSamplePass>& context) const
    {
        context.template output<"out">().publish_coverage(
            context.template input<"in">().coverage());
    }
};

struct BackgroundEventPass {
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

    void tick_block(iv::TickBlockContext<BackgroundEventPass> const&) const {}
    void tock_coverage(iv::TockCoverageContext<BackgroundEventPass>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<BackgroundEventPass>& context) const
    {
        context.template output<"out">().publish_coverage(
            context.template input<"in">().coverage());
    }
};

struct BackgroundMidiSource {
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

    void tick_block(iv::TickBlockContext<BackgroundMidiSource> const&) const {}
    void tock_coverage(iv::TockCoverageContext<BackgroundMidiSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<BackgroundMidiSource>&) const {}
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

struct BackgroundSink {
    static constexpr auto inputs()
    {
        return std::array{iv::random_access_sample_input("in")};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(iv::TickBlockContext<BackgroundSink> const&) const {}
};

struct BackgroundTwoInputPass {
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

    void tick_block(iv::TickBlockContext<BackgroundTwoInputPass> const&) const {}
    void tock_coverage(iv::TockCoverageContext<BackgroundTwoInputPass>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<BackgroundTwoInputPass>&) const {}
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
                "background_ephemeral", {},
                iv::OutputRetention::ephemeral),
            iv::tock_sample_output(
                "background_persisted", {},
                iv::OutputRetention::persisted),
            iv::tock_event_output(
                "background_persisted_events",
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
        plan->dependencies[0].data,
        graph_jit::detail::PlannedConnectionData::sample);

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
            return region.data
                    == graph_jit::detail::PlannedConnectionData::sample
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
            return region.data
                    == graph_jit::detail::PlannedConnectionData::sample
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
                    && connection.target_port.port_index == target_port
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
    // whole-port storage planner. Storage channel composition must consume
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

    auto storage = graph_jit::detail::build_sample_storage_plan(*plan, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    EXPECT_TRUE(storage->compositions.empty());
    auto const connection_index = static_cast<std::size_t>(std::distance(
        plan->sample_connections.begin(), connection));
    ASSERT_LT(connection_index, storage->connection_representations.size());
    ASSERT_LT(connection_index, storage->connection_channel_bindings.size());
    EXPECT_FALSE(storage->connection_representations[connection_index].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[connection_index].has_value());

    auto const& aliases =
        *storage->connection_channel_bindings[connection_index];
    ASSERT_EQ(aliases.size(), 2u);
    auto const source_group_index = static_cast<std::size_t>(std::distance(
        plan->sample_producer_groups.begin(), source_group));
    auto const latent_group_index = static_cast<std::size_t>(std::distance(
        plan->sample_producer_groups.begin(), latent_group));
    ASSERT_TRUE(storage->producer_groups[source_group_index].has_value());
    ASSERT_TRUE(storage->producer_groups[latent_group_index].has_value());
    EXPECT_EQ(
        aliases[0].representation,
        storage->producer_groups[source_group_index]->canonical_representation);
    EXPECT_EQ(aliases[0].representation_channel, 0u);
    EXPECT_EQ(aliases[0].frame_delay, 7u);
    EXPECT_EQ(
        aliases[1].representation,
        storage->producer_groups[latent_group_index]->canonical_representation);
    EXPECT_EQ(aliases[1].representation_channel, 0u);
    EXPECT_EQ(aliases[1].frame_delay, 2u);

    ASSERT_LT(sink_handle, plan->schedule.bundle_execution_position.size());
    ASSERT_TRUE(plan->schedule.bundle_execution_position[sink_handle].has_value());
    auto const sink_position =
        *plan->schedule.bundle_execution_position[sink_handle];
    EXPECT_GE(
        storage->representations[aliases[0].representation].live_interval.end,
        sink_position);
    EXPECT_GE(
        storage->representations[aliases[1].representation].live_interval.end,
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
                    && connection.target_port.port_index == target_port
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

TEST(GraphJitConnectionPlan, RejectsBackgroundEdgesInWholeSemanticScc)
{
    using namespace iv;
    GraphBuilder sample_graph;
    auto sample_first =
        details::configure_concrete_node<BackgroundSamplePass>(sample_graph);
    auto sample_second =
        details::configure_concrete_node<BackgroundSamplePass>(sample_graph);
    sample_first(sample_second);
    sample_second(static_cast<SamplePortRef>(sample_first).detach(5));
    sample_graph.outputs();

    auto sample_configured = std::move(sample_graph).finish();
    auto sample_plan = graph_jit::detail::build_connection_analysis_plan(
        sample_configured, 64);
    ASSERT_FALSE(sample_plan.has_value());
    EXPECT_NE(sample_plan.error().find("background sample connection"),
        std::string::npos);
    EXPECT_NE(sample_plan.error().find("semantic SCC"), std::string::npos);

    GraphBuilder event_graph;
    auto event_first =
        details::configure_concrete_node<BackgroundEventPass>(event_graph);
    auto event_second =
        details::configure_concrete_node<BackgroundEventPass>(event_graph);
    event_first.connect_event_input(0, event_second.event_port());
    event_second.connect_event_input(0, event_first.event_port().detach(7));
    event_graph.outputs();

    auto event_configured = std::move(event_graph).finish();
    auto event_plan = graph_jit::detail::build_connection_analysis_plan(
        event_configured, 64);
    ASSERT_FALSE(event_plan.has_value());
    EXPECT_NE(event_plan.error().find("background event connection"),
        std::string::npos);
    EXPECT_NE(event_plan.error().find("semantic SCC"), std::string::npos);
}

TEST(GraphJitConnectionPlan, RejectsBackgroundSemanticSelfLoop)
{
    using namespace iv;
    GraphBuilder graph;
    auto node = details::configure_concrete_node<BackgroundSamplePass>(graph);
    node(static_cast<SamplePortRef>(node).detach(3));
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_FALSE(plan.has_value());
    EXPECT_NE(plan.error().find("background sample connection"),
        std::string::npos);
    EXPECT_NE(plan.error().find("semantic SCC"), std::string::npos);
}

TEST(GraphJitConnectionPlan, AllowsRealtimeSccToExportBackgroundData)
{
    using namespace iv;
    GraphBuilder graph;
    auto first = details::configure_concrete_node<MixedAccessPass>(graph);
    auto second = details::configure_concrete_node<PlainSamplePass>(graph);
    auto background_sink = details::configure_concrete_node<BackgroundSink>(graph);
    auto const first_handle = first.node_bundle_handle();
    auto const second_handle = second.node_bundle_handle();
    auto const background_sink_handle = background_sink.node_bundle_handle();
    first(second);
    second(first["realtime"].detach(5));
    background_sink(first["background"]);
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
    ASSERT_TRUE(plan->background.bundle_to_semantic_node[first_handle]);
    ASSERT_TRUE(plan->background.bundle_to_semantic_node[second_handle]);
    ASSERT_TRUE(plan->background.bundle_to_semantic_node[background_sink_handle]);
    auto const first_semantic =
        *plan->background.bundle_to_semantic_node[first_handle];
    auto const second_semantic =
        *plan->background.bundle_to_semantic_node[second_handle];
    auto const sink_semantic =
        *plan->background.bundle_to_semantic_node[background_sink_handle];
    EXPECT_EQ(
        plan->background.semantic_nodes[first_semantic].scc,
        plan->background.semantic_nodes[second_semantic].scc);
    EXPECT_NE(
        plan->background.semantic_nodes[first_semantic].scc,
        plan->background.semantic_nodes[sink_semantic].scc);
    EXPECT_LT(
        plan->background.semantic_nodes[first_semantic].scc,
        plan->background.semantic_nodes[sink_semantic].scc);
    EXPECT_TRUE(plan->background.semantic_sccs[
        plan->background.semantic_nodes[first_semantic].scc].cyclic);
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

TEST(GraphJitConnectionPlan, PartitionsOverlappingSamplePortUsesIntoPortAtoms)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<PersistedStereoSource>(graph);
    auto sequential = details::configure_concrete_node<RealtimeSink>(graph);
    auto random_access = details::configure_concrete_node<BackgroundSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    sequential(source[stereo::left]);
    random_access(source[stereo::right]);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    auto const& subsets = plan->background.sample_source_subsets;
    ASSERT_EQ(subsets.size(), 2u);
    auto const atom_for_channel = [&](std::size_t channel)
        -> graph_jit::SampleSourcePortSubsetPlan const* {
        auto const found = std::ranges::find_if(
            subsets,
            [&](graph_jit::SampleSourcePortSubsetPlan const& subset) {
                return subset.port.node_bundle_handle == source_handle
                    && subset.channels.size() == 1
                    && subset.channels.front().channel == channel;
            });
        return found == subsets.end() ? nullptr : &*found;
    };
    auto const* left = atom_for_channel(stereo::left.channel_index);
    auto const* right = atom_for_channel(stereo::right.channel_index);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_TRUE(left->storage.current_tick);
    EXPECT_FALSE(right->storage.current_tick);
    EXPECT_TRUE(left->storage.capture);
    EXPECT_TRUE(right->storage.capture);
    EXPECT_TRUE(left->storage.persisted_pages);
    EXPECT_TRUE(right->storage.persisted_pages);
    EXPECT_EQ(left->connection_indices.size(), 1u);
    EXPECT_EQ(right->connection_indices.size(), 1u);
    EXPECT_NE(left->connection_indices, right->connection_indices);

    ASSERT_EQ(plan->sample_producer_groups.size(), 1u);
    EXPECT_EQ(plan->sample_producer_groups.front().source_atom_indices.size(), 2u);
    ASSERT_EQ(plan->background.connections.size(), 1u);
    ASSERT_EQ(plan->background.connections.front().source_subsets.size(), 1u);
    EXPECT_EQ(
        plan->background.connections.front().source_subsets.front(),
        static_cast<graph_jit::PortSubsetIndex>(
            right - subsets.data()));

    auto const& storage = plan->background.storage;
    ASSERT_EQ(storage.sample_source_storage.size(), subsets.size());
    auto const left_index = static_cast<graph_jit::PortSubsetIndex>(
        left - subsets.data());
    auto const right_index = static_cast<graph_jit::PortSubsetIndex>(
        right - subsets.data());
    auto canonical_for = [&](graph_jit::PortSubsetIndex subset) {
        return std::ranges::find_if(
            storage.sample_source_storage[subset],
            [&](graph_jit::PortStorageIndex port_index) {
                return storage.ports[port_index].storage
                    == graph_jit::PortStorageKind::
                        persisted_pages;
            });
    };
    auto const left_canonical = canonical_for(left_index);
    auto const right_canonical = canonical_for(right_index);
    ASSERT_NE(
        left_canonical,
        storage.sample_source_storage[left_index].end());
    ASSERT_NE(
        right_canonical,
        storage.sample_source_storage[right_index].end());
    EXPECT_EQ(*left_canonical, *right_canonical);
    EXPECT_EQ(
        storage.ports[*left_canonical].sample_channels,
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

TEST(GraphJitConnectionPlan, BackgroundConnectionsDoNotUseRealtimeStoragePolicy)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<BackgroundSource>(graph);
    auto sink = details::configure_concrete_node<BackgroundSink>(graph);
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
    ASSERT_EQ(plan->background.sample_source_subsets.size(), 1u);
    EXPECT_TRUE(plan->background.sample_source_subsets.front()
        .storage.tick_random_access);
    EXPECT_TRUE(plan->background.sample_source_subsets.front()
        .storage.background_random_access);
    ASSERT_EQ(plan->background.requestable_outputs.size(), 1u);
    EXPECT_FALSE(plan->background.ports[
        plan->background.requestable_outputs.front()].stable_identity);
}

TEST(GraphJitConnectionPlan, RetainsCompleteBackgroundTopologyAndRetention)
{
    using namespace iv;
    GraphBuilder graph;
    auto source =
        details::configure_concrete_node<OutputAccessRetentionModes>(graph);
    auto left = details::configure_concrete_node<BackgroundSamplePass>(graph);
    auto right = details::configure_concrete_node<BackgroundSamplePass>(graph);
    auto join = details::configure_concrete_node<BackgroundTwoInputPass>(graph);
    auto const source_handle = source.node_bundle_handle();
    _annotate_node_source_info(source.node_ref(), "source");
    _annotate_node_source_info(left.node_ref(), "left");
    _annotate_node_source_info(right.node_ref(), "right");
    _annotate_node_source_info(join.node_ref(), "join");

    left(source["background_ephemeral"]);
    right(source["background_ephemeral"]);
    join("a"_P = left, "b"_P = right);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto built = graph_jit::detail::build_connection_analysis_plan(
        configured, 64);
    ASSERT_TRUE(built.has_value()) << (built ? std::string{} : built.error());
    auto const& plan = built->background;

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
        plan.ports,
        [](graph_jit::BackgroundPortPlan const& port) {
            return port.retention.has_value()
                == (port.direction
                    == graph_jit::PortDirection::output);
        }));

    auto port_named = [&](std::string_view name)
        -> graph_jit::BackgroundPortPlan const* {
        auto const found = std::ranges::find_if(
            plan.ports,
            [&](graph_jit::BackgroundPortPlan const& port) {
                return port.name == name;
            });
        return found == plan.ports.end() ? nullptr : &*found;
    };
    auto const* background_ephemeral = port_named("background_ephemeral");
    auto const* background_persisted = port_named("background_persisted");
    auto const* background_persisted_events =
        port_named("background_persisted_events");
    auto const* realtime_persisted = port_named("realtime_persisted");
    EXPECT_EQ(port_named("realtime_ephemeral"), nullptr);
    ASSERT_NE(realtime_persisted, nullptr);
    EXPECT_TRUE(realtime_persisted->persisted_tick_output);
    ASSERT_TRUE(realtime_persisted->retention.has_value());
    EXPECT_EQ(*realtime_persisted->retention, OutputRetention::persisted);
    ASSERT_NE(background_ephemeral, nullptr);
    ASSERT_NE(background_persisted, nullptr);
    ASSERT_NE(background_persisted_events, nullptr);
    ASSERT_TRUE(background_ephemeral->retention.has_value());
    ASSERT_TRUE(background_persisted->retention.has_value());
    ASSERT_TRUE(background_persisted_events->retention.has_value());
    EXPECT_EQ(
        *background_ephemeral->retention, OutputRetention::ephemeral);
    EXPECT_EQ(
        *background_persisted->retention, OutputRetention::persisted);
    EXPECT_EQ(
        *background_persisted_events->retention, OutputRetention::persisted);
    ASSERT_TRUE(background_ephemeral->stable_identity);
    EXPECT_EQ(background_ephemeral->stable_identity->node.graph, "root");
    EXPECT_TRUE(background_ephemeral->stable_identity->node.virtual_node
        .starts_with("source#type:"));
    EXPECT_EQ(
        background_ephemeral->stable_identity->port_name, "background_ephemeral");
    EXPECT_EQ(background_ephemeral->outgoing_connections.size(), 2u);

    auto sample_atom_for_port = [&](std::size_t port)
        -> graph_jit::SampleSourcePortSubsetPlan const* {
        auto const found = std::ranges::find_if(
            plan.sample_source_subsets,
            [&](graph_jit::SampleSourcePortSubsetPlan const& subset) {
                return subset.port.node_bundle_handle == source_handle
                    && subset.port.port_index == port;
            });
        return found == plan.sample_source_subsets.end() ? nullptr : &*found;
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
    EXPECT_TRUE(tick_persisted_atom->storage.capture);
    EXPECT_TRUE(tick_persisted_atom->storage.persisted_pages);
    EXPECT_TRUE(tock_ephemeral_atom->storage.tick_random_access);
    EXPECT_TRUE(tock_ephemeral_atom->storage.background_random_access);
    EXPECT_TRUE(tock_persisted_atom->connection_indices.empty());
    EXPECT_FALSE(tock_persisted_atom->storage.capture);
    EXPECT_TRUE(tock_persisted_atom->storage.persisted_pages);

    auto const persisted_event_atom = std::ranges::find_if(
        plan.event_source_subsets,
        [&](graph_jit::EventSourcePortSubsetPlan const& subset) {
            return subset.port.bundle == source_handle && subset.port.port == 0;
        });
    ASSERT_NE(persisted_event_atom, plan.event_source_subsets.end());
    EXPECT_TRUE(persisted_event_atom->connection_indices.empty());
    EXPECT_TRUE(
        persisted_event_atom->storage.persisted_pages);
}

TEST(GraphJitConnectionPlan, RetainsBackgroundEventConversion)
{
    using namespace iv;
    GraphBuilder graph;
    auto source = details::configure_concrete_node<BackgroundMidiSource>(graph);
    auto target = details::configure_concrete_node<BackgroundEventPass>(graph);
    target.connect_event_input(0, source.event_port());
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto built = graph_jit::detail::build_connection_analysis_plan(
        configured, 64);
    ASSERT_TRUE(built.has_value()) << (built ? std::string{} : built.error());
    ASSERT_EQ(built->background.connections.size(), 1u);
    auto const& connection = built->background.connections.front();
    EXPECT_EQ(connection.kind, PortKind::event);
    EXPECT_EQ(connection.event_source_type, EventTypeId::midi);
    EXPECT_EQ(connection.event_target_type, EventTypeId::trigger);
    EXPECT_TRUE(connection.requires_conversion);
    EXPECT_GT(connection.event_conversion.size(), 0u);
    ASSERT_EQ(connection.source_coverage_ports.size(), 1u);
    ASSERT_EQ(connection.target_coverage_ports.size(), 1u);
    EXPECT_EQ(
        built->background.ports[connection.source_coverage_ports.front()]
            .outgoing_connections,
        (std::vector<graph_jit::BackgroundConnectionIndex>{0}));
    EXPECT_EQ(
        built->background.ports[connection.target_coverage_ports.front()]
            .incoming_connections,
        (std::vector<graph_jit::BackgroundConnectionIndex>{0}));

    auto const& storage = built->background.storage;
    ASSERT_EQ(storage.connections.size(), 1u);
    EXPECT_TRUE(storage.connections.front().direct_events.empty());
    ASSERT_EQ(
        storage.connections.front().event_materializations.size(), 2u);
    std::vector<graph_jit::PortStorageKind> storage_kinds;
    for (auto const materialization :
         storage.connections.front().event_materializations) {
        ASSERT_LT(materialization, storage.event_materializations.size());
        storage_kinds.push_back(
            storage.event_materializations[materialization].storage);
    }
    EXPECT_TRUE(std::ranges::contains(
        storage_kinds,
        graph_jit::PortStorageKind::tick_random_access));
    EXPECT_TRUE(std::ranges::contains(
        storage_kinds,
        graph_jit::PortStorageKind::
            background));
}

TEST(GraphJitConnectionPlan, SharesEquivalentBackgroundEventMaterializations)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<BackgroundMidiSource>(graph);
    auto left = details::configure_concrete_node<BackgroundEventPass>(graph);
    auto right = details::configure_concrete_node<BackgroundEventPass>(graph);
    left.connect_event_input(0, source.event_port());
    right.connect_event_input(0, source.event_port());
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->background.connections.size(), 2u);

    auto const& storage = plan->background.storage;
    ASSERT_EQ(storage.connections.size(), 2u);
    ASSERT_EQ(storage.event_materializations.size(), 2u);
    ASSERT_TRUE(std::ranges::all_of(
        storage.connections,
        [](graph_jit::ConnectionStoragePlan const& connection) {
            return connection.event_materializations.size() == 2;
        }));
    EXPECT_EQ(
        storage.connections[0].event_materializations,
        storage.connections[1].event_materializations);
    for (auto const& materialization : storage.event_materializations) {
        EXPECT_EQ(materialization.connections.size(), 2u);
        EXPECT_EQ(materialization.target_subsets.size(), 2u);
    }
}

TEST(GraphJitConnectionPlan, AddressableEventMaterializationSubsumesSequential)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<BackgroundMidiSource>(graph);
    auto sequential = details::configure_concrete_node<PlainEventPass>(graph);
    auto random_access = details::configure_concrete_node<BackgroundEventPass>(graph);
    sequential.connect_event_input(0, source.event_port());
    random_access.connect_event_input(0, source.event_port());
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->background.connections.size(), 2u);

    auto const& storage = plan->background.storage;
    ASSERT_EQ(storage.event_materializations.size(), 2u);
    ASSERT_EQ(storage.connections[0].event_materializations.size(), 1u);
    ASSERT_EQ(storage.connections[1].event_materializations.size(), 2u);
    auto const materialization = storage.connections[0].event_materializations.front();
    EXPECT_TRUE(std::ranges::contains(
        storage.connections[1].event_materializations, materialization));
    EXPECT_EQ(
        storage.event_materializations[materialization].storage,
        graph_jit::PortStorageKind::tick_random_access);
}

TEST(GraphJitConnectionPlan, ExactEventFanoutAliasesPreparedRepresentations)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<TockTriggerSource>(graph);
    auto sequential = details::configure_concrete_node<PlainEventPass>(graph);
    auto random_access = details::configure_concrete_node<BackgroundEventPass>(graph);
    sequential.connect_event_input(0, source.event_port());
    random_access.connect_event_input(0, source.event_port());
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->background.connections.size(), 2u);

    auto const& storage = plan->background.storage;
    EXPECT_TRUE(storage.event_materializations.empty());
    ASSERT_EQ(storage.connections[0].direct_events.size(), 1u);
    ASSERT_EQ(storage.connections[1].direct_events.size(), 2u);
    auto const sequential_binding =
        storage.connections[0].direct_events.front();
    auto const sequential_representation =
        storage.direct_events[sequential_binding].storage;
    EXPECT_EQ(
        storage.ports[sequential_representation].storage,
        graph_jit::PortStorageKind::tick_random_access);
    EXPECT_TRUE(std::ranges::any_of(
        storage.connections[1].direct_events,
        [&](std::size_t direct_index) {
            return storage.direct_events[direct_index].storage
                == sequential_representation;
        }));
}

TEST(GraphJitConnectionPlan, TockToSequentialUsesPreparedBackgroundDelivery)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<BackgroundSource>(graph);
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

    auto const source_subset = std::ranges::find_if(
        plan->background.sample_source_subsets,
        [&](graph_jit::SampleSourcePortSubsetPlan const& subset) {
            return subset.port.node_bundle_handle == source_handle;
        });
    ASSERT_NE(source_subset, plan->background.sample_source_subsets.end());
    EXPECT_TRUE(source_subset->storage.tick_sequential);
    EXPECT_FALSE(source_subset->storage.tick_random_access);

    ASSERT_EQ(plan->background.tick_sequential_inputs.size(), 1u);
    auto const& port = plan->background.ports[
        plan->background.tick_sequential_inputs.front()];
    EXPECT_TRUE(port.tick_sequential_input);
    EXPECT_FALSE(port.random_access_input);
    EXPECT_FLOAT_EQ(static_cast<float>(port.sample_neutral_value), 0.375f);
}

TEST(GraphJitConnectionPlan, AddressableAtomSubsumesSequentialWindow)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<BackgroundSource>(graph);
    auto sequential = details::configure_concrete_node<RealtimeSink>(graph);
    auto random_access = details::configure_concrete_node<BackgroundSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    sequential(source);
    random_access(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());

    auto const subset = std::ranges::find_if(
        plan->background.sample_source_subsets,
        [&](graph_jit::SampleSourcePortSubsetPlan const& candidate) {
            return candidate.port.node_bundle_handle == source_handle;
        });
    ASSERT_NE(subset, plan->background.sample_source_subsets.end());
    EXPECT_EQ(subset->connection_indices.size(), 2u);
    EXPECT_TRUE(subset->storage.tick_random_access);
    EXPECT_TRUE(subset->storage.background_random_access);
    EXPECT_FALSE(subset->storage.tick_sequential);

    auto const atom_index = static_cast<graph_jit::PortSubsetIndex>(
        std::distance(plan->background.sample_source_subsets.begin(), subset));
    auto const& storage = plan->background.storage;
    ASSERT_EQ(storage.sample_source_storage[atom_index].size(), 2u);
    std::vector<graph_jit::PortStorageKind> storage_kinds;
    for (auto const port_index :
         storage.sample_source_storage[atom_index]) {
        storage_kinds.push_back(storage.ports[port_index].storage);
    }
    EXPECT_TRUE(std::ranges::contains(
        storage_kinds,
        graph_jit::PortStorageKind::tick_random_access));
    EXPECT_TRUE(std::ranges::contains(
        storage_kinds,
        graph_jit::PortStorageKind::
            background));
    EXPECT_FALSE(std::ranges::contains(
        storage_kinds,
        graph_jit::PortStorageKind::tick_sequential));
    ASSERT_EQ(storage.connections.size(), 2u);
    EXPECT_TRUE(std::ranges::all_of(
        storage.connections,
        [](graph_jit::ConnectionStoragePlan const& connection) {
            return !connection.direct_samples.empty()
                && connection.sample_materializations.empty();
        }));
}

TEST(GraphJitConnectionPlan, SharesEquivalentBackgroundSampleMaterializations)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<BackgroundStereoSource>(graph);
    auto left = details::configure_concrete_node<RealtimeSink>(graph);
    auto right = details::configure_concrete_node<RealtimeSink>(graph);
    left(source);
    right(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->background.connections.size(), 2u);

    auto const& storage = plan->background.storage;
    ASSERT_EQ(storage.connections.size(), 2u);
    ASSERT_EQ(storage.sample_materializations.size(), 1u);
    ASSERT_TRUE(std::ranges::all_of(
        storage.connections,
        [](graph_jit::ConnectionStoragePlan const& connection) {
            return connection.direct_samples.empty()
                && connection.sample_materializations.size() == 1;
        }));
    EXPECT_EQ(
        storage.connections[0].sample_materializations,
        storage.connections[1].sample_materializations);
    auto const& materialization = storage.sample_materializations.front();
    EXPECT_EQ(
        materialization.storage,
        graph_jit::PortStorageKind::tick_sequential);
    EXPECT_EQ(materialization.connections.size(), 2u);
    EXPECT_EQ(materialization.target_subsets.size(), 2u);
    EXPECT_EQ(materialization.source_channels.size(), 2u);
}

TEST(GraphJitConnectionPlan, AddressableSampleMaterializationSubsumesSequential)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<BackgroundStereoSource>(graph);
    auto sequential = details::configure_concrete_node<RealtimeSink>(graph);
    auto random_access = details::configure_concrete_node<BackgroundSamplePass>(graph);
    sequential(source);
    random_access(source);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->background.connections.size(), 2u);

    auto const& storage = plan->background.storage;
    ASSERT_EQ(storage.sample_materializations.size(), 2u);
    ASSERT_EQ(storage.connections[0].sample_materializations.size(), 1u);
    ASSERT_EQ(storage.connections[1].sample_materializations.size(), 2u);
    auto const materialization = storage.connections[0].sample_materializations.front();
    EXPECT_TRUE(std::ranges::contains(
        storage.connections[1].sample_materializations, materialization));
    EXPECT_EQ(
        storage.sample_materializations[materialization].storage,
        graph_jit::PortStorageKind::tick_random_access);
}

TEST(GraphJitConnectionPlan, BackgroundSampleMaterializationIsTargetAtomLocal)
{
    using namespace iv;

    GraphBuilder graph;
    auto mono = details::configure_concrete_node<MonoSource>(graph);
    auto stereo_source =
        details::configure_concrete_node<BackgroundStereoSource>(graph);
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
    ASSERT_EQ(plan->background.connections.size(), 1u);

    auto const& storage = plan->background.storage;
    ASSERT_EQ(storage.connections.size(), 1u);
    ASSERT_EQ(storage.connections.front().direct_samples.size(), 1u);
    ASSERT_EQ(storage.connections.front().sample_materializations.size(), 1u);
    ASSERT_EQ(storage.sample_materializations.size(), 1u);

    auto const& direct = storage.direct_samples[
        storage.connections.front().direct_samples.front()];
    EXPECT_EQ(direct.target_channel, 0u);
    EXPECT_EQ(
        storage.ports[direct.storage].storage,
        graph_jit::PortStorageKind::current_tick);

    auto const& materialization = storage.sample_materializations[
        storage.connections.front().sample_materializations.front()];
    EXPECT_EQ(
        materialization.storage,
        graph_jit::PortStorageKind::tick_sequential);
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
    auto tock = details::configure_concrete_node<BackgroundSource>(graph);
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

    std::vector<graph_jit::SampleTargetPortSubsetPlan const*> target_subsets;
    for (auto const& subset : plan->background.sample_target_subsets) {
        if (subset.port.node_bundle_handle == sink_handle) {
            target_subsets.push_back(&subset);
        }
    }
    ASSERT_EQ(target_subsets.size(), 2u);
    EXPECT_TRUE(std::ranges::any_of(
        target_subsets,
        [](graph_jit::SampleTargetPortSubsetPlan const* subset) {
            return subset->channels.size() == 1
                && subset->storage.current_tick;
        }));
    EXPECT_TRUE(std::ranges::any_of(
        target_subsets,
        [](graph_jit::SampleTargetPortSubsetPlan const* subset) {
            return subset->channels.size() == 1
                && subset->storage.tick_sequential;
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

    auto storage = graph_jit::detail::build_sample_storage_plan(*plan, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    auto const tick_group_index = static_cast<std::size_t>(
        std::distance(plan->sample_producer_groups.begin(), tick_group));
    auto const tock_group_index = static_cast<std::size_t>(
        std::distance(plan->sample_producer_groups.begin(), tock_group));
    ASSERT_TRUE(storage->producer_groups[tick_group_index].has_value());
    EXPECT_FALSE(storage->producer_groups[tock_group_index].has_value());
    ASSERT_EQ(storage->connection_representations.size(), 1u);
    EXPECT_FALSE(storage->connection_representations[0].has_value());
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
    ASSERT_GE(plan->background.event_source_subsets.size(), 2u);
    ASSERT_GE(plan->background.event_target_subsets.size(), 2u);
    auto const sink_target_atom = std::ranges::find_if(
        plan->background.event_target_subsets,
        [&](graph_jit::EventTargetPortSubsetPlan const& subset) {
            return subset.port.bundle == sink_handle;
        });
    ASSERT_NE(sink_target_atom, plan->background.event_target_subsets.end());
    EXPECT_EQ(sink_target_atom->source_subsets.size(), 2u);
    EXPECT_TRUE(sink_target_atom->storage.current_tick);
    EXPECT_TRUE(sink_target_atom->storage.tick_sequential);
    auto const disconnected_tick_target_atom = std::ranges::find_if(
        plan->background.event_target_subsets,
        [&](graph_jit::EventTargetPortSubsetPlan const& subset) {
            return subset.port.bundle == tick_handle;
        });
    ASSERT_NE(
        disconnected_tick_target_atom,
        plan->background.event_target_subsets.end());
    EXPECT_TRUE(disconnected_tick_target_atom->source_subsets.empty());
    EXPECT_TRUE(disconnected_tick_target_atom->connection_indices.empty());
    auto const tick_atom = std::ranges::find_if(
        plan->background.event_source_subsets,
        [&](graph_jit::EventSourcePortSubsetPlan const& subset) {
            return subset.port.bundle == tick_handle;
        });
    auto const tock_atom = std::ranges::find_if(
        plan->background.event_source_subsets,
        [&](graph_jit::EventSourcePortSubsetPlan const& subset) {
            return subset.port.bundle == tock_handle;
        });
    ASSERT_NE(tick_atom, plan->background.event_source_subsets.end());
    ASSERT_NE(tock_atom, plan->background.event_source_subsets.end());
    EXPECT_TRUE(tick_atom->storage.current_tick);
    EXPECT_TRUE(tock_atom->storage.tick_sequential);

    auto const& storage = plan->background.storage;
    ASSERT_EQ(storage.connections.size(), 1u);
    EXPECT_TRUE(storage.connections.front().direct_events.empty());
    ASSERT_EQ(
        storage.connections.front().event_materializations.size(), 1u);
    auto const materialization_index =
        storage.connections.front().event_materializations.front();
    ASSERT_LT(
        materialization_index, storage.event_materializations.size());
    auto const& materialization =
        storage.event_materializations[materialization_index];
    EXPECT_EQ(
        materialization.storage,
        graph_jit::PortStorageKind::current_tick);
    ASSERT_EQ(materialization.inputs.size(), 2u);
    std::vector<graph_jit::PortStorageKind> input_storage_kinds;
    for (auto const port_index : materialization.inputs) {
        input_storage_kinds.push_back(
            storage.ports[port_index].storage);
    }
    EXPECT_TRUE(std::ranges::contains(
        input_storage_kinds,
        graph_jit::PortStorageKind::current_tick));
    EXPECT_TRUE(std::ranges::contains(
        input_storage_kinds,
        graph_jit::PortStorageKind::tick_sequential));
    EXPECT_DOUBLE_EQ(
        storage.ports[materialization.output]
            .max_events_per_index,
        0.75);
}

TEST(GraphJitConnectionPlan, PersistedTickToRandomAccessUsesStoredBoundary)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<OutputAccessRetentionModes>(graph);
    auto sink = details::configure_concrete_node<BackgroundSink>(graph);
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
    ASSERT_TRUE(plan->background.bundle_to_background_node[source_handle]);
    auto const source_node = *plan->background.bundle_to_background_node[source_handle];
    auto const port = std::ranges::find_if(
        plan->background.ports,
        [&](graph_jit::BackgroundPortPlan const& candidate) {
            return candidate.node == source_node
                && candidate.name == "realtime_persisted";
        });
    ASSERT_NE(port, plan->background.ports.end());
    EXPECT_TRUE(port->persisted_tick_output);
    EXPECT_FALSE(port->replayed_tick_output);

    auto const source_subset = std::ranges::find_if(
        plan->background.sample_source_subsets,
        [&](graph_jit::SampleSourcePortSubsetPlan const& subset) {
            return subset.port.node_bundle_handle == source_handle
                && subset.port.port_index == 1;
        });
    ASSERT_NE(source_subset, plan->background.sample_source_subsets.end());
    EXPECT_TRUE(source_subset->storage.capture);
    EXPECT_TRUE(source_subset->storage.persisted_pages);
    EXPECT_FALSE(source_subset->storage.current_tick);
    auto const target_subset = std::ranges::find_if(
        plan->background.sample_target_subsets,
        [&](graph_jit::SampleTargetPortSubsetPlan const& subset) {
            return subset.port.node_bundle_handle == sink_handle;
        });
    ASSERT_NE(target_subset, plan->background.sample_target_subsets.end());
    EXPECT_FALSE(target_subset->storage.capture);
    EXPECT_TRUE(target_subset->storage.persisted_pages);
}

TEST(GraphJitConnectionPlan, IntrinsicTickReplaySuppliesRandomAccess)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<ReplayableSource>(graph);
    auto sink = details::configure_concrete_node<BackgroundSink>(graph);
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
        plan->background.intrinsic_replay_candidates, source_handle));

    ASSERT_TRUE(plan->background.bundle_to_background_node[source_handle]);
    auto const source_node = *plan->background.bundle_to_background_node[source_handle];
    EXPECT_TRUE(plan->background.nodes[source_node].replays_tick);
    EXPECT_TRUE(plan->background.nodes[source_node].uses_replay_forward_coverage);
    EXPECT_TRUE(plan->background.nodes[source_node].uses_replay_reverse_coverage);
    EXPECT_TRUE(plan->background.nodes[source_node].uses_imported_tick_block_for_replay);
}

TEST(GraphJitConnectionPlan, ContextualReplayTraversesSequentialDependencies)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<ReplayableSource>(graph);
    auto pass = details::configure_concrete_node<ReplayablePass>(graph);
    auto sink = details::configure_concrete_node<BackgroundSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    auto const pass_handle = pass.node_bundle_handle();
    pass(source);
    sink(pass);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    auto const source_node = *plan->background.bundle_to_background_node[source_handle];
    auto const pass_node = *plan->background.bundle_to_background_node[pass_handle];
    EXPECT_TRUE(plan->background.nodes[source_node].replays_tick);
    EXPECT_TRUE(plan->background.nodes[pass_node].replays_tick);
    ASSERT_EQ(plan->background.background_evaluation_order.size(), 2u);
    auto const source_position = std::ranges::find(
        plan->background.background_evaluation_order, source_node);
    auto const pass_position = std::ranges::find(
        plan->background.background_evaluation_order, pass_node);
    ASSERT_NE(source_position, plan->background.background_evaluation_order.end());
    ASSERT_NE(pass_position, plan->background.background_evaluation_order.end());
    EXPECT_LT(source_position, pass_position);
    EXPECT_TRUE(std::ranges::any_of(
        plan->background.background_dependencies,
        [&](graph_jit::BackgroundDependencyPlan const& dependency) {
            return dependency.source_node == source_node
                && dependency.target_node == pass_node
                && dependency.kind
                    == graph_jit::BackgroundDependencyKind::replay_sequential;
        }));

    auto const replay_connection = std::ranges::find_if(
        plan->background.connections,
        [&](graph_jit::BackgroundConnectionPlan const& connection) {
            return connection.kind == PortKind::sample
                && std::ranges::any_of(
                    connection.sample_target_channels,
                    [&](SampleInputChannelId channel) {
                        return channel.bundle == pass_handle;
                    });
        });
    ASSERT_NE(replay_connection, plan->background.connections.end());
    auto const replay_connection_index = static_cast<std::size_t>(
        std::distance(plan->background.connections.begin(), replay_connection));
    auto const& storage = plan->background.storage;
    ASSERT_LT(replay_connection_index, storage.connections.size());
    auto const& storage_connection =
        storage.connections[replay_connection_index];
    ASSERT_EQ(storage_connection.direct_samples.size(), 2u);
    std::vector<graph_jit::PortStorageKind> storage_kinds;
    for (auto const direct_index : storage_connection.direct_samples) {
        ASSERT_LT(direct_index, storage.direct_samples.size());
        storage_kinds.push_back(storage.ports[
            storage.direct_samples[direct_index].storage].storage);
    }
    EXPECT_TRUE(std::ranges::contains(
        storage_kinds,
        graph_jit::PortStorageKind::current_tick));
    EXPECT_TRUE(std::ranges::contains(
        storage_kinds,
        graph_jit::PortStorageKind::
            background));
}

TEST(GraphJitConnectionPlan, TockDependencyCanFeedSynthesizedReplay)
{
    using namespace iv;

    GraphBuilder graph;
    auto source = details::configure_concrete_node<BackgroundSource>(graph);
    auto pass = details::configure_concrete_node<ReplayablePass>(graph);
    auto sink = details::configure_concrete_node<BackgroundSink>(graph);
    auto const source_handle = source.node_bundle_handle();
    auto const pass_handle = pass.node_bundle_handle();
    pass(source);
    sink(pass);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    auto const source_node = *plan->background.bundle_to_background_node[source_handle];
    auto const pass_node = *plan->background.bundle_to_background_node[pass_handle];
    EXPECT_TRUE(plan->background.nodes[source_node].authored_tock_execution);
    EXPECT_TRUE(plan->background.nodes[pass_node].replays_tick);
    auto const source_position = std::ranges::find(
        plan->background.background_evaluation_order, source_node);
    auto const pass_position = std::ranges::find(
        plan->background.background_evaluation_order, pass_node);
    ASSERT_NE(source_position, plan->background.background_evaluation_order.end());
    ASSERT_NE(pass_position, plan->background.background_evaluation_order.end());
    EXPECT_LT(source_position, pass_position);

    auto const replay_connection = std::ranges::find_if(
        plan->background.connections,
        [&](graph_jit::BackgroundConnectionPlan const& connection) {
            return connection.kind == PortKind::sample
                && std::ranges::any_of(
                    connection.sample_target_channels,
                    [&](SampleInputChannelId channel) {
                        return channel.bundle == pass_handle;
                    });
        });
    ASSERT_NE(replay_connection, plan->background.connections.end());
    auto const replay_connection_index = static_cast<std::size_t>(
        std::distance(plan->background.connections.begin(), replay_connection));
    auto const& storage = plan->background.storage;
    auto const& storage_connection =
        storage.connections[replay_connection_index];
    ASSERT_EQ(storage_connection.direct_samples.size(), 2u);
    std::vector<graph_jit::PortStorageKind> storage_kinds;
    for (auto const direct_index : storage_connection.direct_samples) {
        storage_kinds.push_back(storage.ports[
            storage.direct_samples[direct_index].storage].storage);
    }
    EXPECT_TRUE(std::ranges::contains(
        storage_kinds,
        graph_jit::PortStorageKind::tick_sequential));
    EXPECT_TRUE(std::ranges::contains(
        storage_kinds,
        graph_jit::PortStorageKind::
            background));
}

TEST(GraphJitConnectionPlan, PersistedTickBoundaryStopsContextualReplayTraversal)
{
    using namespace iv;

    GraphBuilder graph;
    auto live = details::configure_concrete_node<MonoSource>(graph);
    auto recorder = details::configure_concrete_node<PersistedTickPass>(graph);
    auto replay = details::configure_concrete_node<ReplayablePass>(graph);
    auto sink = details::configure_concrete_node<BackgroundSink>(graph);
    auto const recorder_handle = recorder.node_bundle_handle();
    auto const replay_handle = replay.node_bundle_handle();
    recorder(live);
    replay(recorder);
    sink(replay);
    graph.outputs();

    auto configured = std::move(graph).finish();
    auto plan = graph_jit::detail::build_connection_analysis_plan(configured, 64);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    auto const recorder_node = *plan->background.bundle_to_background_node[recorder_handle];
    auto const replay_node = *plan->background.bundle_to_background_node[replay_handle];
    EXPECT_FALSE(plan->background.nodes[recorder_node].replays_tick);
    EXPECT_TRUE(plan->background.nodes[replay_node].replays_tick);
    EXPECT_TRUE(std::ranges::any_of(
        plan->background.background_dependencies,
        [&](graph_jit::BackgroundDependencyPlan const& dependency) {
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
    auto sample_sink = details::configure_concrete_node<BackgroundSink>(sample_graph);
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
    auto event_sink = details::configure_concrete_node<BackgroundEventPass>(event_graph);
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
    auto sink = details::configure_concrete_node<BackgroundSink>(graph);
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
    auto sink = details::configure_concrete_node<BackgroundSink>(graph);
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
    auto sink = details::configure_concrete_node<BackgroundSink>(graph);
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
