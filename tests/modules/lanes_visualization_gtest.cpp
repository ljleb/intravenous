#include <intravenous/bridge.h>
#include <intravenous/runtime/lanes_visualization.h>

#include <intravenous/runtime/lanes_visualization_events.h>
#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/runtime/task_runner_events.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace {
iv::InternedString intern(std::string_view value)
{
    return iv::InternedString::from_view(value);
}

struct VisualizationTestState {
    std::unordered_map<std::uint64_t, iv::LaneVisualizationOutputDescriptor> output_descriptors {};
    std::unordered_map<std::uint64_t, iv::OwnedSampleBlock> compiled_samples {};
    std::unordered_map<std::uint64_t, std::vector<iv::TimedEvent>> compiled_events {};
    std::vector<iv::LaneViewContentUpdate> updates {};
    iv::TimelineLaneBatchUpdate last_batch {};
    void handle_lane_output_query(
        iv::LaneId lane,
        iv::LanesVisualizationLaneOutputQueryBuilder &builder) const
    {
        if (auto const it = output_descriptors.find(lane.value);
            it != output_descriptors.end()) {
            builder.succeed(it->second);
        }
    }
    void handle_compiled_sample_window(
        iv::LaneId lane,
        size_t,
        size_t,
        size_t point_count,
        iv::LanesVisualizationCompiledSampleWindowBuilder &builder) const
    {
        auto const it = compiled_samples.find(lane.value);
        if (it == compiled_samples.end()) {
            return;
        }
        iv::CompiledSampleWindow window;
        for (size_t i = 0; i < point_count; ++i) {
            window.primary.push_back(it->second.samples[i % it->second.samples.size()].value);
        }
        builder.succeed(std::move(window));
    }
    void handle_compiled_event_window(
        iv::LaneId lane,
        size_t,
        size_t,
        iv::LanesVisualizationCompiledEventWindowBuilder &builder) const
    {
        if (auto const it = compiled_events.find(lane.value);
            it != compiled_events.end()) {
            builder.succeed(it->second);
        }
    }
    void handle_timeline_batch(iv::TimelineLaneBatchUpdate const &batch)
    {
        last_batch = batch;
    }
    void handle_lane_view_content_updated(iv::LaneViewContentUpdate const &update)
    {
        updates.push_back(update);
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(
    visualization_test_state_bridge,
    iv::LanesVisualization,
    VisualizationTestState);
IV_DEFINE_BRIDGE(visualization_test_state_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    visualization_test_state_bridge,
    iv_runtime_lanes_visualization_lane_output_query_event,
    &VisualizationTestState::handle_lane_output_query)

IV_SUBSCRIBE_LINKER_EVENT(
    visualization_test_state_bridge,
    iv_runtime_lanes_visualization_compiled_sample_window_requested_event,
    &VisualizationTestState::handle_compiled_sample_window)

IV_SUBSCRIBE_LINKER_EVENT(
    visualization_test_state_bridge,
    iv_runtime_lanes_visualization_compiled_event_window_requested_event,
    &VisualizationTestState::handle_compiled_event_window)

IV_SUBSCRIBE_LINKER_EVENT(
    visualization_test_state_bridge,
    iv_runtime_lanes_visualization_timeline_batch_requested_event,
    &VisualizationTestState::handle_timeline_batch)

IV_SUBSCRIBE_LINKER_EVENT(
    visualization_test_state_bridge,
    iv_runtime_lane_view_content_updated_event,
    &VisualizationTestState::handle_lane_view_content_updated)

struct VisualizationTestBindings {
    iv::LanesVisualization source {std::nullopt, 512};
    visualization_test_state_bridge::scope scope;

    explicit VisualizationTestBindings(VisualizationTestState &state)
        : scope(source, state)
    {}
};
} // namespace

namespace iv {

TEST(LanesVisualizationTest, PublishesExactCompiledSampleWindowForVisibleLane)
{
    VisualizationTestState state;
    VisualizationTestBindings bindings(state);

    state.output_descriptors[42] = LaneVisualizationOutputDescriptor{
        .config = sample_output_port("test", LanePortDomain::compiled, SampleStreamLayout::interleaved),
        .sample_channel_type = ChannelTypeId::stereo,
        .subscribes_to_compiled_output_changes = true,
    };
    state.compiled_samples[42] = OwnedSampleBlock{
        .samples = { Sample{ 1.0f }, Sample{ 10.0f }, Sample{ 2.0f }, Sample{ 20.0f } },
        .channel_layout = ChannelLayout{
            .channel_type = ChannelTypeId::stereo,
            .sample_layout = SampleStreamLayout::interleaved,
        },
        .frame_count = 2,
    };

    LanesVisualization visualization(std::nullopt, 512);
    visualization.handle_lane_views_updated(LaneViewResult{
        .view_id = intern("view-1"),
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{
                .lane_id = intern("lane-42"),
                .runtime_lane = LaneId{42},
            } },
        },
        .first_sample_index = 0,
        .last_sample_index = 100,
        .display_sample_count = 3,
    });

    visualization.publish_now();

    ASSERT_EQ(state.updates.size(), 1u);
    EXPECT_EQ(state.updates.front().view_id.str(), "view-1");
    ASSERT_EQ(state.updates.front().lanes.size(), 1u);
    EXPECT_EQ(state.updates.front().lanes.front().lane_id.str(), "lane-42");
    EXPECT_EQ(state.updates.front().lanes.front().adapter_type, "samples");
    ASSERT_TRUE(state.updates.front().lanes.front().compiled_sample_window.has_value());
    EXPECT_EQ(state.updates.front().lanes.front().compiled_sample_window->primary.size(), 3u);
    EXPECT_EQ(state.updates.front().lanes.front().compiled_sample_window->primary[0], 1.0f);
    EXPECT_EQ(state.updates.front().lanes.front().compiled_sample_window->primary[2], 2.0f);
    EXPECT_EQ(state.updates.front().lanes.front().compiled_window_first_sample_index, 0u);
    EXPECT_EQ(state.updates.front().lanes.front().compiled_window_last_sample_index, 100u);
}

TEST(LanesVisualizationTest, PublishesCompiledEventDataForVisibleLanes)
{
    VisualizationTestState state;
    VisualizationTestBindings bindings(state);

    state.output_descriptors[7] = LaneVisualizationOutputDescriptor{
        .config = event_output_port("test", LanePortDomain::compiled),
        .subscribes_to_compiled_output_changes = true,
    };
    state.compiled_events[7] = {
        TimedEvent{ .time = 10, .value = TriggerEvent{} },
        TimedEvent{ .time = 20, .value = TriggerEvent{} },
    };

    LanesVisualization visualization(std::nullopt, 512);
    visualization.handle_lane_views_updated(LaneViewResult{
        .view_id = intern("view-2"),
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{
                .lane_id = intern("lane-7"),
                .runtime_lane = LaneId{7},
            } },
        },
        .first_sample_index = 0,
        .last_sample_index = 100,
        .display_sample_count = 0,
    });

    visualization.publish_now();

    ASSERT_EQ(state.updates.size(), 1u);
    ASSERT_EQ(state.updates.front().lanes.size(), 1u);
    EXPECT_EQ(state.updates.front().lanes.front().lane_id.str(), "lane-7");
    EXPECT_EQ(state.updates.front().lanes.front().adapter_type, "events");
    EXPECT_EQ(state.updates.front().lanes.front().events.size(), 2u);
}

TEST(LanesVisualizationTest, ClosedViewStopsPublishingUpdates)
{
    VisualizationTestState state;
    VisualizationTestBindings bindings(state);

    state.output_descriptors[42] = LaneVisualizationOutputDescriptor{
        .config = sample_output_port("test", LanePortDomain::compiled),
        .sample_channel_type = ChannelTypeId::mono,
        .subscribes_to_compiled_output_changes = true,
    };
    state.compiled_samples[42] = OwnedSampleBlock{
        .samples = { Sample{ 1.0f } },
        .channel_layout = ChannelLayout{
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        },
        .frame_count = 1,
    };

    LanesVisualization visualization(std::nullopt, 512);
    visualization.handle_lane_views_updated(LaneViewResult{
        .view_id = intern("view-1"),
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{
                .lane_id = intern("lane-42"),
                .runtime_lane = LaneId{42},
            } },
        },
        .first_sample_index = 0,
        .last_sample_index = 100,
        .display_sample_count = 1,
    });
    visualization.publish_now();
    ASSERT_EQ(state.updates.size(), 1u);

    state.updates.clear();
    visualization.handle_lane_view_closed(intern("view-1"));
    visualization.publish_now();
    EXPECT_TRUE(state.updates.empty());
}

TEST(LanesVisualizationTest, RealtimeSampleLaneQueuesTimelineBatchOnPassFinished)
{
    VisualizationTestState state;
    VisualizationTestBindings bindings(state);

    state.output_descriptors[10] = LaneVisualizationOutputDescriptor{
        .config = sample_output_port("test", LanePortDomain::realtime),
        .sample_channel_type = ChannelTypeId::mono,
    };

    LanesVisualization visualization(std::nullopt, 8);
    visualization.handle_lane_views_updated(LaneViewResult{
        .view_id = intern("view-rt"),
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{
                .lane_id = intern("lane-10"),
                .runtime_lane = LaneId{10},
            } },
        },
    });

    visualization.handle_task_runner_after_pass(TasksRunnerAfterPass{});

    ASSERT_EQ(state.last_batch.upserts.size(), 1u);
    ASSERT_EQ(state.last_batch.connections_to_add.size(), 1u);
    EXPECT_EQ(state.last_batch.connections_to_add.front().source.value, 10u);
}

TEST(LanesVisualizationTest, ClosingViewRemovesRealtimeVisualizationLaneOnNextPass)
{
    VisualizationTestState state;
    VisualizationTestBindings bindings(state);

    state.output_descriptors[10] = LaneVisualizationOutputDescriptor{
        .config = sample_output_port("test", LanePortDomain::realtime),
        .sample_channel_type = ChannelTypeId::mono,
    };

    LanesVisualization visualization(std::nullopt, 8);
    visualization.handle_lane_views_updated(LaneViewResult{
        .view_id = intern("view-rt"),
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{
                .lane_id = intern("lane-10"),
                .runtime_lane = LaneId{10},
            } },
        },
    });

    visualization.handle_task_runner_after_pass(TasksRunnerAfterPass{});
    ASSERT_EQ(state.last_batch.upserts.size(), 1u);
    auto const vis_lane = state.last_batch.upserts.front().lane;

    state.last_batch = {};
    visualization.handle_lane_view_closed(intern("view-rt"));
    visualization.handle_task_runner_after_pass(TasksRunnerAfterPass{});

    ASSERT_EQ(state.last_batch.removals.size(), 1u);
    EXPECT_EQ(state.last_batch.removals.front(), vis_lane);
}

TEST(LanesVisualizationTest, RepeatedIdenticalRealtimeViewUpdatesDoNotDuplicateTimelineUpserts)
{
    VisualizationTestState state;
    VisualizationTestBindings bindings(state);

    state.output_descriptors[10] = LaneVisualizationOutputDescriptor{
        .config = sample_output_port("test", LanePortDomain::realtime),
        .sample_channel_type = ChannelTypeId::mono,
    };

    LanesVisualization visualization(std::nullopt, 8);
    LaneViewResult view{
        .view_id = intern("view-rt"),
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{
                .lane_id = intern("lane-10"),
                .runtime_lane = LaneId{10},
            } },
        },
    };
    visualization.handle_lane_views_updated(view);
    visualization.handle_task_runner_after_pass(TasksRunnerAfterPass{});
    ASSERT_EQ(state.last_batch.upserts.size(), 1u);

    state.last_batch = {};
    visualization.handle_lane_views_updated(view);
    visualization.handle_task_runner_after_pass(TasksRunnerAfterPass{});
    EXPECT_TRUE(state.last_batch.upserts.empty());
    EXPECT_TRUE(state.last_batch.connections_to_add.empty());
}

TEST(LanesVisualizationTest, RealtimeLaneKindChangesAreReclassifiedAcrossPasses)
{
    VisualizationTestState state;
    VisualizationTestBindings bindings(state);

    state.output_descriptors[10] = LaneVisualizationOutputDescriptor{
        .config = sample_output_port("test", LanePortDomain::realtime),
        .sample_channel_type = ChannelTypeId::mono,
    };

    LanesVisualization visualization(std::nullopt, 8);
    LaneViewResult view{
        .view_id = intern("view-rt"),
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{
                .lane_id = intern("lane-10"),
                .runtime_lane = LaneId{10},
            } },
        },
    };
    visualization.handle_lane_views_updated(view);
    visualization.handle_task_runner_after_pass(TasksRunnerAfterPass{});
    ASSERT_EQ(state.last_batch.upserts.size(), 1u);
    auto const old_vis_lane = state.last_batch.upserts.front().lane;
    EXPECT_EQ(state.last_batch.connections_to_add.front().input.kind, PortKind::sample);

    state.output_descriptors[10] = LaneVisualizationOutputDescriptor{
        .config = event_output_port("test", LanePortDomain::realtime),
    };
    state.last_batch = {};
    visualization.handle_lane_views_updated(view);
    visualization.handle_task_runner_after_pass(TasksRunnerAfterPass{});

    ASSERT_EQ(state.last_batch.removals.size(), 1u);
    EXPECT_EQ(state.last_batch.removals.front(), old_vis_lane);
    ASSERT_EQ(state.last_batch.upserts.size(), 1u);
    EXPECT_EQ(state.last_batch.connections_to_add.front().input.kind, PortKind::event);
}

} // namespace iv
