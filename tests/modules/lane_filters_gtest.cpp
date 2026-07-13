#include <intravenous/linker_event.h>
#include <intravenous/query/lane_query_dataset.h>
#include <intravenous/query/lane_query_schema.h>
#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/lane_filters_events.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {
struct FakeLaneRecord {
    std::uint64_t lane_id = 0;
    std::unordered_map<std::string, bool> unit_values {};
    std::unordered_map<std::string, int> int_values {};
    std::unordered_map<std::string, float> float_values {};
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

    [[nodiscard]] iv::query::LaneQuerySchema const &schema() const override
    {
        return schema_;
    }

    [[nodiscard]] size_t lane_count() const override
    {
        return lanes_.size();
    }

    [[nodiscard]] std::uint64_t lane_id_at(size_t lane_index) const override
    {
        return lanes_.at(lane_index).lane_id;
    }

    [[nodiscard]] bool in_filter(size_t, std::string_view) const override
    {
        return false;
    }

    [[nodiscard]] bool has_unit(size_t lane_index, iv::query::LaneQueryPropertyId property) const override
    {
        return lanes_.at(lane_index).unit_values.contains(schema_.key_of(property));
    }

    [[nodiscard]] std::optional<int> int_value(size_t lane_index, iv::query::LaneQueryPropertyId property) const override
    {
        auto const &values = lanes_.at(lane_index).int_values;
        auto const it = values.find(schema_.key_of(property));
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] std::optional<float> float_value(size_t lane_index, iv::query::LaneQueryPropertyId property) const override
    {
        auto const &values = lanes_.at(lane_index).float_values;
        auto const it = values.find(schema_.key_of(property));
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

struct LaneFiltersWitness {
    std::vector<iv::LaneFiltersChanged> changes {};
};

LaneFiltersWitness *g_lane_filters_witness = nullptr;

iv::LaneMetadata metadata_for_lane(iv::LaneId lane_id)
{
    iv::LaneMetadata metadata;
    metadata.set_int("lane_id", static_cast<int>(lane_id.value));
    return metadata;
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

iv::TimelineLanesChanged make_change(
    std::shared_ptr<iv::query::LaneQueryDataset const> dataset,
    iv::query::LaneQuerySchemaChange schema_change = {})
{
    return iv::TimelineLanesChanged{
        .lane_set_changed = true,
        .dataset = std::move(dataset),
        .schema_change = std::move(schema_change),
        .metadata_for_lane = metadata_for_lane,
        .outputs_for_lanes = no_outputs,
    };
}

IV_SUBSCRIBE_LINKER_EVENT(
    iv::LaneFiltersChangedEvent,
    iv_runtime_lane_filters_changed_event,
    +[](iv::LaneFiltersChanged const &change) {
        if (g_lane_filters_witness != nullptr) {
            g_lane_filters_witness->changes.push_back(change);
        }
    });

class LaneFiltersTest : public ::testing::Test {
protected:
    iv::LaneFilters filters;
    LaneFiltersWitness witness {};

    void SetUp() override
    {
        g_lane_filters_witness = &witness;
    }

    void TearDown() override
    {
        g_lane_filters_witness = nullptr;
    }
};
} // namespace

TEST_F(LaneFiltersTest, StoreFilterWithoutDatasetDoesNotEmitSnapshot)
{
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "graph_input.default",
        .query_source = "graph_input",
    });

    EXPECT_TRUE(witness.changes.empty());
}

TEST_F(LaneFiltersTest, StoreFilterWithDatasetEmitsSnapshot)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 7);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{
            {.lane_id = 11, .unit_values = {{"graph_input", true}}},
            {.lane_id = 12, .unit_values = {}},
        });

    filters.handle_timeline_lanes_changed(make_change(dataset));
    witness.changes.clear();

    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "graph_input.default",
        .query_source = "graph_input",
    });

    ASSERT_EQ(witness.changes.size(), 1u);
    ASSERT_EQ(witness.changes.front().results.size(), 1u);
    auto const *snapshot =
        std::get_if<iv::FilteredLanesSnapshot>(&witness.changes.front().results.front().outcome);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->filter_name, "graph_input.default");
    EXPECT_EQ(snapshot->query_source, "graph_input");
    ASSERT_EQ(snapshot->lane_ids.size(), 1u);
    EXPECT_EQ(snapshot->lane_ids.front().value, 11u);
    EXPECT_EQ(snapshot->revision, 7u);
}

TEST_F(LaneFiltersTest, RemoveFilterDeletesStoredState)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{{.lane_id = 11, .unit_values = {{"graph_input", true}}}});

    filters.handle_timeline_lanes_changed(make_change(dataset));
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "graph_input.default",
        .query_source = "graph_input",
    });
    witness.changes.clear();

    filters.remove_filter("graph_input.default");
    filters.handle_timeline_lanes_changed(make_change(dataset));

    ASSERT_EQ(witness.changes.size(), 1u);
    EXPECT_TRUE(witness.changes.front().results.empty());
}

TEST_F(LaneFiltersTest, TimelineChangeRefreshesAllStoredFilters)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
        {"knob", iv::query::LaneQueryValueType::unit},
    }, 3);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{
            {.lane_id = 11, .unit_values = {{"graph_input", true}}},
            {.lane_id = 12, .unit_values = {{"knob", true}}},
        });

    filters.handle_timeline_lanes_changed(make_change(dataset));
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "graph_input.default",
        .query_source = "graph_input",
    });
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "graph_input.knobs",
        .query_source = "knob",
    });
    witness.changes.clear();

    filters.handle_timeline_lanes_changed(make_change(dataset));

    ASSERT_EQ(witness.changes.size(), 1u);
    EXPECT_TRUE(witness.changes.front().all_filters_changed);
    ASSERT_EQ(witness.changes.front().results.size(), 2u);
}

TEST_F(LaneFiltersTest, DeltaWithoutDatasetRetainsTheLastLaneSnapshot)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{{.lane_id = 11, .unit_values = {{"graph_input", true}}}});

    filters.handle_timeline_lanes_changed(make_change(dataset));
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "all-graph-inputs",
        .query_source = "graph_input",
    });
    witness.changes.clear();

    auto delta = make_change(nullptr);
    delta.lane_set_changed = false;
    filters.handle_timeline_lanes_changed(delta);

    ASSERT_EQ(witness.changes.size(), 1u);
    ASSERT_EQ(witness.changes.front().results.size(), 1u);
    auto const *snapshot = std::get_if<iv::FilteredLanesSnapshot>(&witness.changes.front().results.front().outcome);
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->lane_ids.size(), 1u);
    EXPECT_EQ(snapshot->lane_ids.front().value, 11u);
}

TEST_F(LaneFiltersTest, SchemaChangeRebindsStoredFilters)
{
    auto const old_schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto const new_schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 2);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        old_schema,
        std::vector<FakeLaneRecord>{{.lane_id = 11, .unit_values = {{"graph_input", true}}}});
    auto rebound_dataset = std::make_shared<FakeLaneQueryDataset>(
        new_schema,
        std::vector<FakeLaneRecord>{{.lane_id = 12, .unit_values = {{"graph_input", true}}}});

    filters.handle_timeline_lanes_changed(make_change(dataset));
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "graph_input.default",
        .query_source = "graph_input",
    });
    witness.changes.clear();

    filters.handle_timeline_lanes_changed(make_change(
        rebound_dataset,
        iv::query::LaneQuerySchemaChange{
            .changed = true,
            .old_revision = 1,
            .new_revision = 2,
        }));

    ASSERT_EQ(witness.changes.size(), 1u);
    EXPECT_TRUE(witness.changes.front().schema_change.changed);
    ASSERT_EQ(witness.changes.front().results.size(), 1u);
    auto const *snapshot =
        std::get_if<iv::FilteredLanesSnapshot>(&witness.changes.front().results.front().outcome);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->revision, 2u);
    ASSERT_EQ(snapshot->lane_ids.size(), 1u);
    EXPECT_EQ(snapshot->lane_ids.front().value, 12u);
}

TEST_F(LaneFiltersTest, StoreFilterPublishesParseError)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{{.lane_id = 11, .unit_values = {{"graph_input", true}}}});

    filters.handle_timeline_lanes_changed(make_change(dataset));
    witness.changes.clear();

    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "broken",
        .query_source = "(",
    });

    ASSERT_EQ(witness.changes.size(), 1u);
    ASSERT_EQ(witness.changes.front().results.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<iv::LaneFilterError>(witness.changes.front().results.front().outcome));
}

TEST_F(LaneFiltersTest, UnknownReferencedFilterPublishesError)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{{.lane_id = 11, .unit_values = {{"graph_input", true}}}});

    filters.handle_timeline_lanes_changed(make_change(dataset));
    witness.changes.clear();

    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "dependent",
        .query_source = "filter.missing",
    });

    ASSERT_EQ(witness.changes.size(), 1u);
    ASSERT_EQ(witness.changes.front().results.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<iv::LaneFilterError>(witness.changes.front().results.front().outcome));
}

TEST_F(LaneFiltersTest, ReferencedFilterMembershipCanDriveDependentFilter)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{
            {.lane_id = 11, .unit_values = {{"graph_input", true}}},
            {.lane_id = 12, .unit_values = {}},
        });

    filters.handle_timeline_lanes_changed(make_change(dataset));
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "graph_input.default",
        .query_source = "graph_input",
    });
    witness.changes.clear();

    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "dependent",
        .query_source = "filter.graph_input.default",
    });

    ASSERT_EQ(witness.changes.size(), 1u);
    ASSERT_EQ(witness.changes.front().results.size(), 2u);

    auto const dependent_it = std::find_if(
        witness.changes.front().results.begin(),
        witness.changes.front().results.end(),
        [](iv::LaneFilterResult const &result) {
            return result.filter_name == "dependent";
        });
    ASSERT_NE(dependent_it, witness.changes.front().results.end());
    auto const *dependent_snapshot = std::get_if<iv::FilteredLanesSnapshot>(&dependent_it->outcome);
    ASSERT_NE(dependent_snapshot, nullptr);
    ASSERT_EQ(dependent_snapshot->lane_ids.size(), 1u);
    EXPECT_EQ(dependent_snapshot->lane_ids.front().value, 11u);
}

TEST_F(LaneFiltersTest, CyclicReferencedFiltersPublishError)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{{.lane_id = 11, .unit_values = {{"graph_input", true}}}});

    filters.handle_timeline_lanes_changed(make_change(dataset));
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "a",
        .query_source = "filter.b",
    });
    witness.changes.clear();

    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "b",
        .query_source = "filter.a",
    });

    ASSERT_EQ(witness.changes.size(), 1u);
    ASSERT_EQ(witness.changes.front().results.size(), 2u);
    EXPECT_TRUE(std::all_of(
        witness.changes.front().results.begin(),
        witness.changes.front().results.end(),
        [](iv::LaneFilterResult const &result) {
            return std::holds_alternative<iv::LaneFilterError>(result.outcome);
        }));
}

TEST_F(LaneFiltersTest, ParseErrorPersistsAcrossTimelineRefresh)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{{.lane_id = 11, .unit_values = {{"graph_input", true}}}});

    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "broken",
        .query_source = "(",
    });
    witness.changes.clear();

    filters.handle_timeline_lanes_changed(make_change(dataset));

    ASSERT_EQ(witness.changes.size(), 1u);
    ASSERT_EQ(witness.changes.front().results.size(), 1u);
    auto const *error =
        std::get_if<iv::LaneFilterError>(&witness.changes.front().results.front().outcome);
    ASSERT_NE(error, nullptr);
    EXPECT_FALSE(error->message.empty());
}

TEST_F(LaneFiltersTest, RemovingReferencedFilterRecomputesDependentsAsErrors)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    }, 1);
    auto dataset = std::make_shared<FakeLaneQueryDataset>(
        schema,
        std::vector<FakeLaneRecord>{{.lane_id = 11, .unit_values = {{"graph_input", true}}}});

    filters.handle_timeline_lanes_changed(make_change(dataset));
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "graph_input.default",
        .query_source = "graph_input",
    });
    filters.store_filter(iv::LaneFilterStoredRequest{
        .filter_name = "dependent",
        .query_source = "filter.graph_input.default",
    });
    witness.changes.clear();

    filters.remove_filter("graph_input.default");

    ASSERT_EQ(witness.changes.size(), 1u);
    ASSERT_EQ(witness.changes.front().results.size(), 1u);
    ASSERT_EQ(witness.changes.front().results.front().filter_name, "dependent");
    EXPECT_TRUE(std::holds_alternative<iv::LaneFilterError>(witness.changes.front().results.front().outcome));
}
