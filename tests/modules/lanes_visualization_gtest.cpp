#include <intravenous/runtime/lanes_visualization.h>

#include <intravenous/runtime/lanes_visualization_events.h>
#include <intravenous/runtime/task_runner_events.h>

#include <gtest/gtest.h>

#include <optional>
#include <unordered_map>

namespace {
struct VisualizationTestState {
    std::unordered_map<std::uint64_t, iv::LaneOutputConfig> output_configs {};
    std::unordered_map<std::uint64_t, std::vector<iv::Sample>> compiled_samples {};
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
        auto const it = g_state->output_configs.find(lane.value);
        if (it == g_state->output_configs.end()) {
            return;
        }
        builder.succeed(it->second);
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LanesVisualizationCompiledSampleWindowRequestedEvent,
    iv_runtime_lanes_visualization_compiled_sample_window_requested_event,
    +[](iv::LaneId lane,
        size_t /*first*/,
        size_t /*last*/,
        size_t /*count*/,
        iv::LanesVisualizationCompiledSampleWindowBuilder &builder) {
        if (g_state == nullptr) {
            return;
        }
        auto const it = g_state->compiled_samples.find(lane.value);
        if (it == g_state->compiled_samples.end()) {
            return;
        }
        builder.succeed(it->second);
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

TEST(LanesVisualizationTest, PublishesCompiledSampleDataForVisibleLanes)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_configs[42] = CompiledSampleLaneOutputConfig{ .name = "test" };
    state.compiled_samples[42] = { Sample{ 1.0f }, Sample{ 2.0f }, Sample{ 3.0f } };

    LanesVisualization visualization(std::nullopt, 2, 512);
    visualization.handle_lane_views_updated(LaneViewResult{
        .view_id = "view-1",
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{ .lane_id = 42 } },
        },
        .first_sample_index = 0,
        .last_sample_index = 100,
        .display_sample_count = 3,
    });

    visualization.publish_now();

    ASSERT_EQ(state.updates.size(), 1u);
    EXPECT_EQ(state.updates.front().view_id, "view-1");
    ASSERT_EQ(state.updates.front().lanes.size(), 1u);
    EXPECT_EQ(state.updates.front().lanes.front().lane_id, 42u);
    EXPECT_EQ(state.updates.front().lanes.front().adapter_type, "samples");
    EXPECT_EQ(
        state.updates.front().lanes.front().samples,
        (std::vector<Sample::storage>{ 1.0f, 2.0f, 3.0f }));

    g_state = nullptr;
}

TEST(LanesVisualizationTest, PublishesCompiledEventDataForVisibleLanes)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_configs[7] = CompiledEventLaneOutputConfig{ .name = "test" };
    state.compiled_events[7] = {
        TimedEvent{ .time = 10, .value = TriggerEvent{} },
        TimedEvent{ .time = 20, .value = TriggerEvent{} },
    };

    LanesVisualization visualization(std::nullopt, 2, 512);
    visualization.handle_lane_views_updated(LaneViewResult{
        .view_id = "view-2",
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{ .lane_id = 7 } },
        },
        .first_sample_index = 0,
        .last_sample_index = 100,
        .display_sample_count = 0,
    });

    visualization.publish_now();

    ASSERT_EQ(state.updates.size(), 1u);
    ASSERT_EQ(state.updates.front().lanes.size(), 1u);
    EXPECT_EQ(state.updates.front().lanes.front().lane_id, 7u);
    EXPECT_EQ(state.updates.front().lanes.front().adapter_type, "events");
    EXPECT_EQ(state.updates.front().lanes.front().events.size(), 2u);

    g_state = nullptr;
}

TEST(LanesVisualizationTest, ClosedViewStopsPublishingUpdates)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_configs[42] = CompiledSampleLaneOutputConfig{ .name = "test" };
    state.compiled_samples[42] = { Sample{ 1.0f } };

    LanesVisualization visualization(std::nullopt, 2, 512);
    visualization.handle_lane_views_updated(LaneViewResult{
        .view_id = "view-1",
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{ .lane_id = 42 } },
        },
        .first_sample_index = 0,
        .last_sample_index = 100,
        .display_sample_count = 1,
    });
    visualization.publish_now();
    ASSERT_EQ(state.updates.size(), 1u);

    state.updates.clear();
    visualization.handle_lane_view_closed("view-1");
    visualization.publish_now();
    EXPECT_TRUE(state.updates.empty());

    g_state = nullptr;
}

TEST(LanesVisualizationTest, RealtimeSampleLaneQueuesTimelineBatchOnPassFinished)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_configs[10] = RealtimeSampleLaneOutputConfig{ .name = "test" };

    LanesVisualization visualization(std::nullopt, 2, 8);
    visualization.handle_lane_views_updated(LaneViewResult{
        .view_id = "view-rt",
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{ .lane_id = 10 } },
        },
    });

    // Pass finished should trigger timeline batch with one upsert and one connection
    visualization.handle_task_runner_pass_finished(TaskRunnerPassFinished{});

    ASSERT_EQ(state.last_batch.upserts.size(), 1u);
    ASSERT_EQ(state.last_batch.connections_to_add.size(), 1u);
    EXPECT_EQ(state.last_batch.connections_to_add.front().source.value, 10u);

    g_state = nullptr;
}

TEST(LanesVisualizationTest, ClosingViewRemovesRealtimeVisualizationLaneOnNextPass)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_configs[10] = RealtimeSampleLaneOutputConfig{ .name = "test" };

    LanesVisualization visualization(std::nullopt, 2, 8);
    visualization.handle_lane_views_updated(LaneViewResult{
        .view_id = "view-rt",
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{ .lane_id = 10 } },
        },
    });

    visualization.handle_task_runner_pass_finished(TaskRunnerPassFinished{});
    ASSERT_EQ(state.last_batch.upserts.size(), 1u);
    auto const vis_lane = state.last_batch.upserts.front().lane;

    state.last_batch = {};
    visualization.handle_lane_view_closed("view-rt");
    visualization.handle_task_runner_pass_finished(TaskRunnerPassFinished{});

    ASSERT_EQ(state.last_batch.removals.size(), 1u);
    EXPECT_EQ(state.last_batch.removals.front(), vis_lane);

    g_state = nullptr;
}

TEST(LanesVisualizationTest, RepeatedIdenticalRealtimeViewUpdatesDoNotDuplicateTimelineUpserts)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_configs[10] = RealtimeSampleLaneOutputConfig{ .name = "test" };

    LanesVisualization visualization(std::nullopt, 2, 8);
    LaneViewResult view{
        .view_id = "view-rt",
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{ .lane_id = 10 } },
        },
    };
    visualization.handle_lane_views_updated(view);
    visualization.handle_task_runner_pass_finished(TaskRunnerPassFinished{});
    ASSERT_EQ(state.last_batch.upserts.size(), 1u);

    state.last_batch = {};
    visualization.handle_lane_views_updated(view);
    visualization.handle_task_runner_pass_finished(TaskRunnerPassFinished{});
    EXPECT_TRUE(state.last_batch.upserts.empty());
    EXPECT_TRUE(state.last_batch.connections_to_add.empty());

    g_state = nullptr;
}

TEST(LanesVisualizationTest, RealtimeLaneKindChangesAreReclassifiedAcrossPasses)
{
    VisualizationTestState state;
    g_state = &state;

    state.output_configs[10] = RealtimeSampleLaneOutputConfig{ .name = "test" };

    LanesVisualization visualization(std::nullopt, 2, 8);
    LaneViewResult view{
        .view_id = "view-rt",
        .lanes = LaneQueryResult{
            .lanes = { LaneInfo{ .lane_id = 10 } },
        },
    };
    visualization.handle_lane_views_updated(view);
    visualization.handle_task_runner_pass_finished(TaskRunnerPassFinished{});
    ASSERT_EQ(state.last_batch.upserts.size(), 1u);
    auto const old_vis_lane = state.last_batch.upserts.front().lane;
    EXPECT_EQ(state.last_batch.connections_to_add.front().input.kind, PortKind::sample);

    state.output_configs[10] = RealtimeEventLaneOutputConfig{ .name = "test" };
    state.last_batch = {};
    visualization.handle_lane_views_updated(view);
    visualization.handle_task_runner_pass_finished(TaskRunnerPassFinished{});

    ASSERT_EQ(state.last_batch.removals.size(), 1u);
    EXPECT_EQ(state.last_batch.removals.front(), old_vis_lane);
    ASSERT_EQ(state.last_batch.upserts.size(), 1u);
    EXPECT_EQ(state.last_batch.connections_to_add.front().input.kind, PortKind::event);

    g_state = nullptr;
}

} // namespace iv
