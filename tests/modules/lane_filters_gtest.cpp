#include <intravenous/bridge.h>
#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/lane_filters_events.h>

#include <gtest/gtest.h>

#include <vector>

namespace {
struct LaneFiltersWitness {
    std::vector<iv::LaneFiltersChanged> changes {};

    void handle_lane_filters_changed(iv::LaneFiltersChanged const &change)
    {
        changes.push_back(change);
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(lane_filters_witness_bridge, iv::LaneFilters, LaneFiltersWitness);
IV_DEFINE_BRIDGE(lane_filters_witness_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    lane_filters_witness_bridge,
    iv_runtime_lane_filters_changed_event,
    &LaneFiltersWitness::handle_lane_filters_changed)

class LaneFiltersTest : public ::testing::Test {
protected:
    iv::LaneFilters filters;
    LaneFiltersWitness witness {};
    lane_filters_witness_bridge::scope witness_scope {filters, witness};
};
} // namespace

TEST_F(LaneFiltersTest, DisconnectedFilterUpdatesDoNotPublishSnapshots)
{
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "graph_input.default",
        .query_source = "graph_input",
    });
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "broken",
        .query_source = "(",
    });
    filters.remove_filter("graph_input.default");
    filters.remove_filter("broken");

    EXPECT_TRUE(witness.changes.empty());
}
