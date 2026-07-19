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
};

VisualizationTestState *g_state = nullptr;

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LanesVisualizationLaneOutputQueryEvent,
    iv_runtime_lanes_visualization_lane_output_query_event,
    +[](iv::LaneId lane, iv::LanesVisualizationLaneOutputQueryBuilder &builder) {
        if (g_state == nullptr) {
            return;
        }
        auto const it = g_state->output_descriptors.find(lane.value);
        if (it == g_state->output_descriptors.end()) {
            return;
        }
        builder.succeed(it->second);
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LanesVisualizationCompiledSampleLevelRequestedEvent,
    iv_runtime_lanes_visualization_compiled_sample_level_requested_event,
    +[](iv::LaneId lane,
        size_t /*first*/,
        size_t /*last*/,
        iv::LanesVisualizationCompiledSampleLevelBuilder &builder) {
        if (g_state == nullptr) {
            return;
        }
        auto const it = g_state->compiled_samples.find(lane.value);
        if (it == g_state->compiled_samples.end()) {
            return;
        }
        iv::Sample::storage level = 0.0f;
        for (auto const sample : it->second.samples) {
            level = std::max(level, std::abs(sample.value));
        }
        builder.succeed(level);
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LanesVisualizationCompiledEventWindowRequestedEvent,
    iv_runtime_lanes_visualization_compiled_event_window_requested_event,
    +[](iv::LaneId lane,
        size_t /*first*/,
        size_t /*last*/,
        iv::LanesVisualizationCompiledEventWindowBuilder &builder) {
        if (g_state == nullptr) {
            return;
        }
        auto const it = g_state->compiled_events.find(lane.value);
        if (it == g_state->compiled_events.end()) {
            return;
        }
        builder.succeed(it->second);
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LanesVisualizationTimelineBatchRequestedEvent,
    iv_runtime_lanes_visualization_timeline_batch_requested_event,
    +[](iv::TimelineLaneBatchUpdate const &batch) {
        if (g_state == nullptr) {
            return;
        }
        g_state->last_batch = batch;
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LaneViewContentUpdatedEvent,
    iv_runtime_lane_view_content_updated_event,
    +[](iv::LaneViewContentUpdate const &update) {
        if (g_state == nullptr) {
            return;
        }
        g_state->updates.push_back(update);
    });
} // namespace

namespace iv {

TEST(LanesVisualizationTest, PublishesOneCompiledSampleLevelForVisibleLane)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_descriptors[42] = LaneVisualizationOutputDescriptor{
        .config = CompiledSampleLaneOutputConfig{ .name = "test", .sample_layout = SampleStreamLayout::interleaved },
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
    EXPECT_EQ(state.updates.front().lanes.front().adapter_type, "level");
    ASSERT_TRUE(state.updates.front().lanes.front().peak_level.has_value());
    EXPECT_EQ(*state.updates.front().lanes.front().peak_level, 20.0f);

    g_state = nullptr;
}

TEST(LanesVisualizationTest, PublishesCompiledEventDataForVisibleLanes)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_descriptors[7] = LaneVisualizationOutputDescriptor{
        .config = CompiledEventLaneOutputConfig{ .name = "test" },
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

    g_state = nullptr;
}

TEST(LanesVisualizationTest, ClosedViewStopsPublishingUpdates)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_descriptors[42] = LaneVisualizationOutputDescriptor{
        .config = CompiledSampleLaneOutputConfig{ .name = "test" },
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

    g_state = nullptr;
}

TEST(LanesVisualizationTest, RealtimeSampleLaneQueuesTimelineBatchOnPassFinished)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_descriptors[10] = LaneVisualizationOutputDescriptor{
        .config = RealtimeSampleLaneOutputConfig{ .name = "test" },
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

    // Pass finished should trigger timeline batch with one upsert and one connection
    visualization.handle_task_runner_after_pass(TasksRunnerAfterPass{});

    ASSERT_EQ(state.last_batch.upserts.size(), 1u);
    ASSERT_EQ(state.last_batch.connections_to_add.size(), 1u);
    EXPECT_EQ(state.last_batch.connections_to_add.front().source.value, 10u);

    g_state = nullptr;
}

TEST(LanesVisualizationTest, ClosingViewRemovesRealtimeVisualizationLaneOnNextPass)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_descriptors[10] = LaneVisualizationOutputDescriptor{
        .config = RealtimeSampleLaneOutputConfig{ .name = "test" },
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

    g_state = nullptr;
}

TEST(LanesVisualizationTest, RepeatedIdenticalRealtimeViewUpdatesDoNotDuplicateTimelineUpserts)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_descriptors[10] = LaneVisualizationOutputDescriptor{
        .config = RealtimeSampleLaneOutputConfig{ .name = "test" },
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

    g_state = nullptr;
}

TEST(LanesVisualizationTest, RealtimeLaneKindChangesAreReclassifiedAcrossPasses)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_descriptors[10] = LaneVisualizationOutputDescriptor{
        .config = RealtimeSampleLaneOutputConfig{ .name = "test" },
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
        .config = RealtimeEventLaneOutputConfig{ .name = "test" },
    };
    state.last_batch = {};
    visualization.handle_lane_views_updated(view);
    visualization.handle_task_runner_after_pass(TasksRunnerAfterPass{});

    ASSERT_EQ(state.last_batch.removals.size(), 1u);
    EXPECT_EQ(state.last_batch.removals.front(), old_vis_lane);
    ASSERT_EQ(state.last_batch.upserts.size(), 1u);
    EXPECT_EQ(state.last_batch.connections_to_add.front().input.kind, PortKind::event);

    g_state = nullptr;
}

} // namespace iv
