#include "linker_event.h"
#include "query/lane_query_dataset.h"
#include "query/lane_query_schema.h"
#include "runtime/lane_filters.h"
#include "runtime/lane_filters_events.h"
#include "runtime/lane_filters_lane_views_bridge.h"
#include "runtime/lane_views.h"
#include "runtime/lane_views_events.h"
#include "runtime/timeline_events.h"
#include "runtime/timeline_lane_filters_bridge.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {
struct FakeLaneRecord {
    std::uint64_t lane_id = 0;
    std::unordered_map<std::string, bool> unit_values {};
};

class FakeLaneQueryDataset final : public iv::query::LaneQueryDataset {
    iv::query::LaneQuerySchema schema_;
    std::vector<FakeLaneRecord> lanes_;

public:
    FakeLaneQueryDataset(
        iv::query::LaneQuerySchema schema,
        std::vector<FakeLaneRecord> lanes)
        : schema_(std::move(schema))
        , lanes_(std::move(lanes))
    {}

    [[nodiscard]] iv::query::LaneQuerySchema const &schema() const override { return schema_; }
    [[nodiscard]] size_t lane_count() const override { return lanes_.size(); }
    [[nodiscard]] std::uint64_t lane_id_at(size_t lane_index) const override { return lanes_.at(lane_index).lane_id; }
    [[nodiscard]] bool in_filter(size_t, std::string_view) const override { return false; }
    [[nodiscard]] bool has_unit(size_t lane_index, iv::query::LaneQueryPropertyId property) const override
    {
        return lanes_.at(lane_index).unit_values.contains(schema_.key_of(property));
    }
    [[nodiscard]] std::optional<int> int_value(size_t, iv::query::LaneQueryPropertyId) const override
    {
        return std::nullopt;
    }
    [[nodiscard]] std::optional<float> float_value(size_t, iv::query::LaneQueryPropertyId) const override
    {
        return std::nullopt;
    }
};

struct BridgeWitness {
    std::vector<iv::LaneFiltersChanged> filter_changes {};
    std::vector<iv::LaneViewResult> view_updates {};
};

BridgeWitness *g_bridge_witness = nullptr;

iv::LaneMetadata metadata_for_lane(iv::LaneId)
{
    return {};
}

std::vector<iv::TimelineLaneOutputs> no_outputs(std::vector<iv::LaneId> const &lanes)
{
    std::vector<iv::TimelineLaneOutputs> outputs;
    outputs.reserve(lanes.size());
    for (auto lane : lanes) {
        outputs.push_back(iv::TimelineLaneOutputs{
            .lane = lane,
            .outputs = {},
        });
    }
    return outputs;
}

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LaneFiltersChangedEvent,
    iv_runtime_lane_filters_changed_event,
    +[](iv::LaneFiltersChanged const &change) {
        if (g_bridge_witness != nullptr) {
            g_bridge_witness->filter_changes.push_back(change);
        }
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LaneViewsUpdatedEvent,
    iv_runtime_lane_views_updated_event,
    +[](iv::LaneViewResult const &update) {
        if (g_bridge_witness != nullptr) {
            g_bridge_witness->view_updates.push_back(update);
        }
    });

class LaneFilterBridgesTest : public ::testing::Test {
protected:
    iv::LaneFilters filters;
    iv::LaneViews views;
    BridgeWitness witness {};

    void SetUp() override
    {
        g_bridge_witness = &witness;
        iv::bind_timeline_lane_filters_bridge(filters);
        iv::bind_lane_filters_lane_views_bridge(filters, views);
    }

    void TearDown() override
    {
        iv::unbind_lane_filters_lane_views_bridge(filters, views);
        iv::unbind_timeline_lane_filters_bridge(filters);
        g_bridge_witness = nullptr;
    }
};
} // namespace

TEST_F(LaneFilterBridgesTest, TimelineLaneFiltersBridgeForwardsTimelineChanges)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{{.lane_id = 41, .unit_values = {{"graph_input", true}}}});

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_lane_filter_stored_event,
        iv::LaneFilterStoredRequest{
            .filter_name = "graph_input.default",
            .query_source = "graph_input",
        });
    witness.filter_changes.clear();

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_timeline_lanes_changed_event,
        iv::TimelineLanesChanged{
            .lane_set_changed = true,
            .dataset = dataset,
            .metadata_for_lane = metadata_for_lane,
            .outputs_for_lanes = no_outputs,
        });

    ASSERT_EQ(witness.filter_changes.size(), 1u);
    ASSERT_EQ(witness.filter_changes.front().results.size(), 1u);
    auto const *snapshot =
        std::get_if<iv::FilteredLanesSnapshot>(&witness.filter_changes.front().results.front().outcome);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->filter_name, "graph_input.default");
}

TEST_F(LaneFilterBridgesTest, LaneFiltersLaneViewsBridgeForwardsStoredFilterEvents)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{{.lane_id = 41, .unit_values = {{"graph_input", true}}}});
    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_timeline_lanes_changed_event,
        iv::TimelineLanesChanged{
            .lane_set_changed = true,
            .dataset = dataset,
            .metadata_for_lane = metadata_for_lane,
            .outputs_for_lanes = no_outputs,
        });
    witness.filter_changes.clear();

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_lane_filter_stored_event,
        iv::LaneFilterStoredRequest{
            .filter_name = "graph_input.default",
            .query_source = "graph_input",
        });

    ASSERT_EQ(witness.filter_changes.size(), 1u);
    ASSERT_EQ(witness.filter_changes.front().results.size(), 1u);
    auto const *snapshot =
        std::get_if<iv::FilteredLanesSnapshot>(&witness.filter_changes.front().results.front().outcome);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->filter_name, "graph_input.default");
}

TEST_F(LaneFilterBridgesTest, LaneFiltersLaneViewsBridgeForwardsFilterResultsToViews)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{{.lane_id = 41, .unit_values = {{"graph_input", true}}}});
    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_timeline_lanes_changed_event,
        iv::TimelineLanesChanged{
            .lane_set_changed = true,
            .dataset = dataset,
            .metadata_for_lane = metadata_for_lane,
            .outputs_for_lanes = no_outputs,
        });

    (void)views.open_view(iv::LaneViewRequest{
        .view_id = "abc",
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
    EXPECT_EQ(witness.view_updates.front().view_id, "abc");
}

TEST_F(LaneFilterBridgesTest, LaneFiltersLaneViewsBridgeForwardsFilterErrorsToViews)
{
    (void)views.open_view(iv::LaneViewRequest{
        .view_id = "abc",
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
    EXPECT_EQ(witness.view_updates.front().view_id, "abc");
    ASSERT_TRUE(witness.view_updates.front().lanes.error_message.has_value());
    EXPECT_EQ(*witness.view_updates.front().lanes.error_message, "bad filter");
}
