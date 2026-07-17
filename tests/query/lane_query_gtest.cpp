#include <intravenous/query/lane_query_engine.h>
#include <intravenous/query/lane_query_completion.h>
#include <intravenous/query/lane_query_parser.h>
#include <intravenous/query/lane_query_program.h>
#include <intravenous/query/lane_query_schema.h>
#include <intravenous/query/lane_query_tokenizer.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
struct FakeLaneRecord {
    std::uint64_t lane_id = 0;
    std::unordered_map<std::string, bool> unit_values {};
    std::unordered_map<std::string, int> int_values {};
    std::unordered_map<std::string, float> float_values {};
    std::unordered_map<std::string, bool> filter_memberships {};
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

    [[nodiscard]] bool in_filter(size_t lane_index, std::string_view filter_name) const override
    {
        return lanes_.at(lane_index).filter_memberships.contains(std::string(filter_name));
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

std::vector<size_t> execute(
    std::string_view source,
    iv::query::LaneQuerySchema const &schema,
    iv::query::LaneQueryDataset const &dataset)
{
    iv::query::LaneQueryParser parser;
    auto const raw = parser.parse(source);
    auto const bound = iv::query::bind_lane_query_ast(raw, schema);
    return iv::query::execute_lane_query(bound, dataset).matching_lane_indexes;
}
} // namespace

TEST(LaneQuery, ExistsMatchesTypedProperties)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
        {"port_ordinal", iv::query::LaneQueryValueType::int_},
    });
    FakeLaneQueryDataset dataset{
        schema,
        {
            {.lane_id = 1, .unit_values = {{"graph_input", true}}, .int_values = {{"port_ordinal", 1}}},
            {.lane_id = 2, .unit_values = {}, .int_values = {}},
        }};

    EXPECT_EQ(execute("graph_input", schema, dataset), (std::vector<size_t>{0}));
    EXPECT_EQ(execute("port_ordinal", schema, dataset), (std::vector<size_t>{0}));
}

TEST(LaneQuery, AndOrAndNotCompose)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
        {"knob", iv::query::LaneQueryValueType::unit},
    });
    FakeLaneQueryDataset dataset{
        schema,
        {
            {.lane_id = 1, .unit_values = {{"graph_input", true}, {"knob", true}}},
            {.lane_id = 2, .unit_values = {{"graph_input", true}}},
            {.lane_id = 3, .unit_values = {{"knob", true}}},
        }};

    EXPECT_EQ(execute("graph_input knob", schema, dataset), (std::vector<size_t>{0}));
    EXPECT_EQ(execute("graph_input | knob", schema, dataset), (std::vector<size_t>{0, 1, 2}));
    EXPECT_EQ(execute("graph_input !knob", schema, dataset), (std::vector<size_t>{1}));
}

TEST(LaneQuery, IntegerAndFloatRangesMatch)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"port_ordinal", iv::query::LaneQueryValueType::int_},
        {"gain", iv::query::LaneQueryValueType::float_},
    });
    FakeLaneQueryDataset dataset{
        schema,
        {
            {.lane_id = 1, .int_values = {{"port_ordinal", 1}}, .float_values = {{"gain", 0.25f}}},
            {.lane_id = 2, .int_values = {{"port_ordinal", 3}}, .float_values = {{"gain", 0.75f}}},
            {.lane_id = 3, .int_values = {{"port_ordinal", 5}}, .float_values = {{"gain", 1.25f}}},
        }};

    EXPECT_EQ(execute("port_ordinal=2..5", schema, dataset), (std::vector<size_t>{1, 2}));
    EXPECT_EQ(execute("gain=0.5..1.0", schema, dataset), (std::vector<size_t>{1}));
}

TEST(LaneQuery, GroupedPathExpansionWorks)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"tag.red", iv::query::LaneQueryValueType::unit},
        {"tag.blue", iv::query::LaneQueryValueType::unit},
        {"tag.green", iv::query::LaneQueryValueType::unit},
    });
    FakeLaneQueryDataset dataset{
        schema,
        {
            {.lane_id = 1, .unit_values = {{"tag.red", true}, {"tag.blue", true}}},
            {.lane_id = 2, .unit_values = {{"tag.green", true}}},
            {.lane_id = 3, .unit_values = {{"tag.red", true}}},
        }};

    EXPECT_EQ(execute("tag.(red blue | green)", schema, dataset), (std::vector<size_t>{0, 1}));
}

TEST(LaneQuery, IntegerProjectionMatchesProjectedValues)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"group", iv::query::LaneQueryValueType::unit},
        {"port", iv::query::LaneQueryValueType::int_},
        {"target", iv::query::LaneQueryValueType::int_},
    });
    FakeLaneQueryDataset dataset{
        schema,
        {
            {.lane_id = 1, .unit_values = {{"group", true}}, .int_values = {{"port", 3}, {"target", 9}}},
            {.lane_id = 2, .unit_values = {}, .int_values = {{"target", 3}}},
            {.lane_id = 3, .unit_values = {}, .int_values = {{"target", 5}}},
        }};

    EXPECT_EQ(execute("target=group:port", schema, dataset), (std::vector<size_t>{1}));
}

TEST(LaneQuery, UnitProjectionMatchesPresenceFromProjectedSubset)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"selected", iv::query::LaneQueryValueType::unit},
        {"flag", iv::query::LaneQueryValueType::unit},
    });
    FakeLaneQueryDataset dataset{
        schema,
        {
            {.lane_id = 1, .unit_values = {{"selected", true}, {"flag", true}}},
            {.lane_id = 2, .unit_values = {{"selected", true}}},
            {.lane_id = 3, .unit_values = {{"flag", true}}},
        }};

    EXPECT_EQ(execute("selected=flag:selected", schema, dataset), (std::vector<size_t>{0, 1}));
}

TEST(LaneQuery, UnknownPropertyFailsBinding)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    });
    iv::query::LaneQueryParser parser;
    auto const raw = parser.parse("missing");

    EXPECT_THROW(
        (void)iv::query::bind_lane_query_ast(raw, schema),
        std::runtime_error);
}

TEST(LaneQuery, FilterPropertyMatchesMembership)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"graph_input", iv::query::LaneQueryValueType::unit},
    });
    FakeLaneQueryDataset dataset{
        schema,
        {
            {.lane_id = 1, .filter_memberships = {{"graph_input.default", true}}},
            {.lane_id = 2, .filter_memberships = {}},
        }};

    EXPECT_EQ(execute("filter.graph_input.default", schema, dataset), (std::vector<size_t>{0}));
}

TEST(LaneQuery, FilterPropertyCanBeProjectedAsUnitValue)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"selected", iv::query::LaneQueryValueType::unit},
    });
    FakeLaneQueryDataset dataset{
        schema,
        {
            {.lane_id = 1, .unit_values = {{"selected", true}}, .filter_memberships = {{"graph_input.default", true}}},
            {.lane_id = 2, .unit_values = {{"selected", true}}, .filter_memberships = {}},
            {.lane_id = 3, .unit_values = {}, .filter_memberships = {}},
        }};

    EXPECT_EQ(
        execute("selected=selected:filter.graph_input.default", schema, dataset),
        (std::vector<size_t>{0, 1}));
}

TEST(LaneQueryCompletion, ReplacesTheCurrentPropertyAtom)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"dsp_graph.graph_input", iv::query::LaneQueryValueType::unit},
        {"gain", iv::query::LaneQueryValueType::float_},
    });

    auto const result = iv::query::complete_lane_query("dsp_gr", 6, schema);

    EXPECT_EQ(result.context, iv::query::LaneQueryCompletionContext::property);
    EXPECT_EQ(result.replacement_range.start_offset, 0u);
    EXPECT_EQ(result.replacement_range.end_offset, 6u);
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].label, "dsp_graph.graph_input");
    EXPECT_EQ(result.candidates[0].insert_text, "dsp_graph.graph_input");
}

TEST(LaneQueryCompletion, CompletesOnlyTheSegmentAfterADot)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"tag.blue", iv::query::LaneQueryValueType::unit},
        {"tag.red", iv::query::LaneQueryValueType::unit},
    });

    auto const result = iv::query::complete_lane_query("tag.r", 5, schema);

    EXPECT_EQ(result.context, iv::query::LaneQueryCompletionContext::property);
    EXPECT_EQ(result.replacement_range.start_offset, 4u);
    EXPECT_EQ(result.replacement_range.end_offset, 5u);
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].label, "tag.red");
    EXPECT_EQ(result.candidates[0].insert_text, "red");
}

TEST(LaneQueryCompletion, ConstrainsValueCandidatesByAssignedPropertyType)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"enabled", iv::query::LaneQueryValueType::unit},
        {"gain", iv::query::LaneQueryValueType::float_},
    });

    auto const unit_result = iv::query::complete_lane_query("enabled=", 8, schema);
    EXPECT_EQ(unit_result.context, iv::query::LaneQueryCompletionContext::value);
    ASSERT_EQ(unit_result.candidates.size(), 1u);
    EXPECT_EQ(unit_result.candidates[0].insert_text, "unit");

    auto const numeric_result = iv::query::complete_lane_query("gain=", 5, schema);
    EXPECT_EQ(numeric_result.context, iv::query::LaneQueryCompletionContext::value);
    ASSERT_EQ(numeric_result.candidates.size(), 2u);
    EXPECT_EQ(numeric_result.candidates[0].insert_text, "0");
    EXPECT_EQ(numeric_result.candidates[1].insert_text, "0..1");
}

TEST(LaneQueryCompletion, CompletesProjectionSelectors)
{
    auto const schema = iv::query::LaneQuerySchema::from_entries({
        {"group", iv::query::LaneQueryValueType::unit},
        {"target", iv::query::LaneQueryValueType::int_},
    });

    auto const result = iv::query::complete_lane_query("target=group:t", 14, schema);

    EXPECT_EQ(result.context, iv::query::LaneQueryCompletionContext::projection_selector);
    EXPECT_EQ(result.replacement_range.start_offset, 13u);
    EXPECT_EQ(result.replacement_range.end_offset, 14u);
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].insert_text, "target");
}

TEST(LaneQueryTokenizer, RetainsUtf8ByteRangesAndToleratesIncompleteEditorText)
{
    auto const tokens = iv::query::tokenize_lane_query("gain=1");
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].start_offset, 0u);
    EXPECT_EQ(tokens[0].end_offset, 4u);
    EXPECT_EQ(tokens[1].start_offset, 4u);
    EXPECT_EQ(tokens[1].end_offset, 5u);
    EXPECT_EQ(tokens[2].start_offset, 5u);
    EXPECT_EQ(tokens[2].end_offset, 6u);

    auto const incomplete = iv::query::tokenize_lane_query_tolerant("gain=-");
    ASSERT_EQ(incomplete.size(), 4u);
    EXPECT_EQ(incomplete[2].kind, iv::query::LaneQueryToken::Kind::invalid);
    EXPECT_EQ(incomplete[2].start_offset, 5u);
    EXPECT_EQ(incomplete[2].end_offset, 6u);
}
