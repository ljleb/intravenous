#include "runtime/lane_view_service.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

TEST(RuntimeLaneViews, LaneViewServiceTracksViewportAndPushesUpdates)
{
    size_t query_count = 0;
    std::vector<iv::LaneViewResult> updates;
    iv::LaneViewService views(
        [&query_count](
            iv::LaneQueryFilter const &filter,
            std::optional<size_t> start_index,
            std::optional<size_t> visible_lane_count) {
            ++query_count;
            EXPECT_EQ(filter.kind, "graphInputs");
            iv::LaneQueryResult result;
            result.start_index = start_index.value_or(0);
            result.visible_lane_count = visible_lane_count.value_or(2);
            result.total_lane_count = 3;
            result.lanes.push_back(iv::LaneInfo{
                .lane_id = static_cast<uint64_t>(result.start_index + 1),
            });
            return result;
        });
    views.set_update_sink([&](iv::LaneViewResult const &update) {
        updates.push_back(update);
    });

    auto opened = views.open_view(iv::LaneViewRequest{
        .view_id = "view",
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.kind = "graphInputs"},
        },
        .start_index = 1,
        .visible_lane_count = 2,
    });

    EXPECT_EQ(opened.view_id, "view");
    EXPECT_EQ(opened.lanes.start_index, 1u);
    EXPECT_EQ(opened.lanes.visible_lane_count, 2u);
    EXPECT_EQ(query_count, 1u);
    EXPECT_TRUE(updates.empty());

    views.mark_lane_changed(iv::LaneId{2});
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].view_id, "view");
    EXPECT_EQ(updates[0].lanes.start_index, 1u);
    EXPECT_EQ(updates[0].lanes.visible_lane_count, 2u);
    EXPECT_EQ(query_count, 2u);
    updates.clear();

    views.mark_lane_changed(iv::LaneId{99});
    EXPECT_TRUE(updates.empty());
    EXPECT_EQ(query_count, 2u);
}

TEST(RuntimeLaneViews, LaneViewServiceCloseStopsPushedUpdates)
{
    std::vector<iv::LaneViewResult> updates;
    iv::LaneViewService views(
        [](iv::LaneQueryFilter const &, std::optional<size_t>, std::optional<size_t>) {
            return iv::LaneQueryResult{};
        });
    views.set_update_sink([&](iv::LaneViewResult const &update) {
        updates.push_back(update);
    });

    views.open_view(iv::LaneViewRequest{
        .view_id = "view",
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.kind = "graphInputs"},
        },
    });
    views.mark_lane_set_changed();
    ASSERT_EQ(updates.size(), 1u);
    updates.clear();
    views.close_view("view");
    views.mark_lane_set_changed();

    EXPECT_TRUE(updates.empty());
}
