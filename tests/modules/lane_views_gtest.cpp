#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_filters_events.h>
#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/lane_views_events.h>
#include <intravenous/runtime/lane_view_service.h>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {
iv::InternedString intern(std::string_view value)
{
    return iv::InternedString::from_view(value);
}

struct LaneViewsWitness {
    std::vector<iv::LaneFilterStoredRequest> stored_filters {};
    std::vector<std::string> removed_filters {};
    std::vector<iv::LaneViewResult> updates {};
};

LaneViewsWitness *g_lane_views_witness = nullptr;

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LaneFilterStoredEvent,
    iv_runtime_lane_filter_stored_event,
    +[](iv::LaneFilterStoredRequest const &request) {
        if (g_lane_views_witness != nullptr) {
            g_lane_views_witness->stored_filters.push_back(request);
        }
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LaneFilterRemovedEvent,
    iv_runtime_lane_filter_removed_event,
    +[](std::string const &filter_name) {
        if (g_lane_views_witness != nullptr) {
            g_lane_views_witness->removed_filters.push_back(filter_name);
        }
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LaneViewsUpdatedEvent,
    iv_runtime_lane_views_updated_event,
    +[](iv::LaneViewResult const &update) {
        if (g_lane_views_witness != nullptr) {
            g_lane_views_witness->updates.push_back(update);
        }
    });

class LaneViewsModuleTest : public ::testing::Test {
protected:
    LaneViewsWitness witness {};

    void SetUp() override
    {
        g_lane_views_witness = &witness;
    }

    void TearDown() override
    {
        g_lane_views_witness = nullptr;
    }
};
} // namespace

TEST(LaneViews, LaneViewServiceTracksViewportAndPushesUpdates)
{
    size_t query_count = 0;
    std::vector<iv::LaneViewResult> updates;
    iv::LaneViewService views(
        [&query_count](
            iv::LaneQueryFilter const &filter,
            std::optional<size_t> start_index,
            std::optional<size_t> visible_lane_count) {
            ++query_count;
            EXPECT_EQ(filter.source, "graph_input");
            iv::LaneQueryResult result;
            result.start_index = start_index.value_or(0);
            result.visible_lane_count = visible_lane_count.value_or(2);
            result.total_lane_count = 3;
            result.lanes.push_back(iv::LaneInfo{
                .lane_id = intern(std::to_string(result.start_index + 1)),
                .runtime_lane = iv::LaneId{result.start_index + 1},
            });
            return result;
        });
    views.set_update_sink([&](iv::LaneViewResult const &update) {
        updates.push_back(update);
    });

    auto opened = views.open_view(iv::LaneViewRequest{
        .view_id = intern("view"),
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "graph_input"},
        },
        .start_index = 1,
        .visible_lane_count = 2,
    });

    EXPECT_EQ(opened.view_id.str(), "view");
    EXPECT_EQ(opened.lanes.start_index, 1u);
    EXPECT_EQ(opened.lanes.visible_lane_count, 2u);
    EXPECT_EQ(query_count, 1u);
    EXPECT_TRUE(updates.empty());

    views.mark_lane_changed(iv::LaneId{2});
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].view_id.str(), "view");
    EXPECT_EQ(updates[0].lanes.start_index, 1u);
    EXPECT_EQ(updates[0].lanes.visible_lane_count, 2u);
    EXPECT_EQ(query_count, 2u);
    updates.clear();

    views.mark_lane_changed(iv::LaneId{99});
    EXPECT_TRUE(updates.empty());
    EXPECT_EQ(query_count, 2u);
}

TEST(LaneViews, LaneViewServiceCloseStopsPushedUpdates)
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
        .view_id = intern("view"),
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "graph_input"},
        },
    });
    views.mark_lane_set_changed();
    ASSERT_EQ(updates.size(), 1u);
    updates.clear();
    views.close_view(intern("view"));
    views.mark_lane_set_changed();

    EXPECT_TRUE(updates.empty());
}

TEST_F(LaneViewsModuleTest, OpenViewStoresFilterAsLaneViewDotId)
{
    iv::LaneViews views;

    auto result = views.open_view(iv::LaneViewRequest{
        .view_id = intern("abc"),
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "graph_input"},
        },
    });

    EXPECT_EQ(result.view_id.str(), "abc");
    ASSERT_EQ(witness.stored_filters.size(), 1u);
    EXPECT_EQ(witness.stored_filters.front().filter_name, "lane_view.abc");
    EXPECT_EQ(witness.stored_filters.front().query_source, "graph_input");
}

TEST_F(LaneViewsModuleTest, UpdateViewReplacesFilterForLaneViewDotId)
{
    iv::LaneViews views;

    (void)views.open_view(iv::LaneViewRequest{
        .view_id = intern("abc"),
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "graph_input"},
        },
    });
    witness.stored_filters.clear();

    (void)views.update_view(iv::LaneViewRequest{
        .view_id = intern("abc"),
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "graph_input knob"},
        },
    });

    ASSERT_EQ(witness.stored_filters.size(), 1u);
    EXPECT_EQ(witness.stored_filters.front().filter_name, "lane_view.abc");
    EXPECT_EQ(witness.stored_filters.front().query_source, "graph_input knob");
}

TEST_F(LaneViewsModuleTest, CloseViewRemovesLaneViewDotIdFilter)
{
    iv::LaneViews views;

    (void)views.open_view(iv::LaneViewRequest{
        .view_id = intern("abc"),
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "graph_input"},
        },
    });
    witness.removed_filters.clear();

    views.close_view("abc");

    ASSERT_EQ(witness.removed_filters.size(), 1u);
    EXPECT_EQ(witness.removed_filters.front(), "lane_view.abc");
}

TEST_F(LaneViewsModuleTest, MatchingSnapshotRefreshesOpenView)
{
    iv::LaneViews views;
    (void)views.open_view(iv::LaneViewRequest{
        .view_id = intern("abc"),
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "graph_input"},
        },
        .visible_lane_count = 8,
    });
    witness.updates.clear();

    views.handle_lane_filters_changed(iv::LaneFiltersChanged{
        .all_filters_changed = true,
        .results = {
            iv::LaneFilterResult{
                .filter_name = "lane_view.abc",
                .query_source = "graph_input",
                .outcome = iv::FilteredLanesSnapshot{
                    .filter_name = "lane_view.abc",
                    .query_source = "graph_input",
                    .revision = 1,
                    .lane_ids = {iv::LaneId{42}},
                },
            },
        },
    });

    ASSERT_EQ(witness.updates.size(), 1u);
    EXPECT_EQ(witness.updates.front().view_id.str(), "abc");
    ASSERT_EQ(witness.updates.front().lanes.lanes.size(), 1u);
    EXPECT_EQ(witness.updates.front().lanes.lanes.front().lane_id.str(), "42");
}

TEST_F(LaneViewsModuleTest, MatchingFilterErrorRefreshesOpenView)
{
    iv::LaneViews views;
    (void)views.open_view(iv::LaneViewRequest{
        .view_id = intern("abc"),
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "graph_input"},
        },
        .visible_lane_count = 8,
    });
    witness.updates.clear();

    views.handle_lane_filters_changed(iv::LaneFiltersChanged{
        .all_filters_changed = true,
        .results = {
            iv::LaneFilterResult{
                .filter_name = "lane_view.abc",
                .query_source = "graph_input",
                .outcome = iv::LaneFilterError{
                    .filter_name = "lane_view.abc",
                    .query_source = "graph_input",
                    .message = "bad filter",
                },
            },
        },
    });

    ASSERT_EQ(witness.updates.size(), 1u);
    EXPECT_EQ(witness.updates.front().view_id.str(), "abc");
    ASSERT_TRUE(witness.updates.front().lanes.error_message.has_value());
    EXPECT_EQ(*witness.updates.front().lanes.error_message, "bad filter");
}
