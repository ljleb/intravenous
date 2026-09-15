#include <intravenous/bridge.h>
#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/lane_filters_events.h>
#include <intravenous/runtime/lane_filters_lane_views_bridge.h>
#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/lane_views_events.h>

#include <gtest/gtest.h>

#include <utility>
#include <variant>
#include <vector>

namespace {
struct BridgeScopeLeft {};
struct BridgeScopeRight {};
IV_DECLARE_BRIDGE(bridge_scope_test_bridge, BridgeScopeLeft, BridgeScopeRight);
IV_DEFINE_BRIDGE(bridge_scope_test_bridge);

iv::InternedString intern(std::string_view value)
{
    return iv::InternedString::from_view(value);
}

struct BridgeWitness {
    std::vector<iv::LaneViewResult> view_updates {};

    void handle_lane_views_updated(iv::LaneViewResult const &update)
    {
        view_updates.push_back(update);
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(lane_views_witness_bridge, iv::LaneViews, BridgeWitness);
IV_DEFINE_BRIDGE(lane_views_witness_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    lane_views_witness_bridge,
    iv_runtime_lane_views_updated_event,
    &BridgeWitness::handle_lane_views_updated)

class LaneFilterBridgesTest : public ::testing::Test {
protected:
    iv::LaneFilters filters;
    iv::LaneViews views;
    iv::lane_filters_lane_views_bridge::scope lane_filters_lane_views_scope {
        &filters, &views
    };
    BridgeWitness witness {};
    lane_views_witness_bridge::scope lane_views_witness_scope {views, witness};
};
} // namespace

TEST(LinkerBridgeScopeTest, RejectsSecondBindingAndTransfersOwnershipOnMove)
{
    BridgeScopeLeft left;
    BridgeScopeRight right;
    auto first_scope = bridge_scope_test_bridge::bind(left, right);

    EXPECT_THROW(
        (void)bridge_scope_test_bridge::bind(left, right),
        std::logic_error);

    auto second_scope = std::move(first_scope);
    EXPECT_EQ(
        bridge_scope_test_bridge::get<BridgeScopeLeft>(),
        &left);
    EXPECT_EQ(
        bridge_scope_test_bridge::get<BridgeScopeRight>(),
        &right);

    first_scope = std::move(second_scope);
    EXPECT_EQ(
        bridge_scope_test_bridge::get<BridgeScopeLeft>(),
        &left);
    EXPECT_EQ(
        bridge_scope_test_bridge::get<BridgeScopeRight>(),
        &right);
}

TEST_F(LaneFilterBridgesTest, LaneFiltersLaneViewsBridgeForwardsFilterResultsToViews)
{
    (void)views.open_view(iv::LaneViewRequest{
        .view_id = intern("abc"),
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "graph_input"},
        },
    });
    witness.view_updates.clear();

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_lane_filters_changed_event,
        iv::LaneFiltersChanged{
            .all_filters_changed = true,
            .results = {
                iv::LaneFilterResult{
                    .filter_name = "lane_view.abc",
                    .query_source = "graph_input",
                    .outcome = iv::FilteredLanesSnapshot{
                        .filter_name = "lane_view.abc",
                        .query_source = "graph_input",
                        .revision = 1,
                        .lane_ids = {iv::LaneId{41}},
                    },
                },
            },
        });

    ASSERT_EQ(witness.view_updates.size(), 1u);
    EXPECT_EQ(witness.view_updates.front().view_id.str(), "abc");
}

TEST_F(LaneFilterBridgesTest, LaneFiltersLaneViewsBridgeForwardsFilterErrorsToViews)
{
    (void)views.open_view(iv::LaneViewRequest{
        .view_id = intern("abc"),
        .query = iv::LaneQuery{
            .filter = iv::LaneQueryFilter{.source = "graph_input"},
        },
    });
    witness.view_updates.clear();

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_lane_filters_changed_event,
        iv::LaneFiltersChanged{
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

    ASSERT_EQ(witness.view_updates.size(), 1u);
    EXPECT_EQ(witness.view_updates.front().view_id.str(), "abc");
    ASSERT_TRUE(witness.view_updates.front().lanes.error_message.has_value());
    EXPECT_EQ(*witness.view_updates.front().lanes.error_message, "bad filter");
}
