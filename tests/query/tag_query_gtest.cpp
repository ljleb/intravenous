#include <intravenous/query/tag_query_engine.h>
#include <intravenous/query/tag_query_completion.h>
#include <intravenous/query/tag_query_parser.h>
#include <intravenous/query/tag_query_program.h>
#include <intravenous/query/tag_query_schema.h>
#include <intravenous/query/tag_query_tokenizer.h>
#include <intravenous/runtime/node_definition_search.h>
#include <intravenous/runtime/node_definition_types.h>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
struct FakeTagRecord {
    std::unordered_map<std::string, bool> unit_values {};
    std::unordered_map<std::string, int> int_values {};
    std::unordered_map<std::string, float> float_values {};
    std::unordered_map<std::string, bool> filter_memberships {};
};

class FakeTagQueryDataset final : public iv::query::TagQueryDataset {
    iv::query::TagQuerySchema schema_;
    std::vector<FakeTagRecord> entries_;

public:
    FakeTagQueryDataset(
        iv::query::TagQuerySchema schema,
        std::vector<FakeTagRecord> entries)
        : schema_(std::move(schema))
        , entries_(std::move(entries))
    {}

    [[nodiscard]] iv::query::TagQuerySchema const &schema() const override
    {
        return schema_;
    }

    [[nodiscard]] size_t entry_count() const override
    {
        return entries_.size();
    }

    [[nodiscard]] bool in_filter(size_t entry_index, std::string_view filter_name) const override
    {
        return entries_.at(entry_index).filter_memberships.contains(std::string(filter_name));
    }

    [[nodiscard]] bool has_unit(size_t entry_index, iv::query::TagQueryPropertyId property) const override
    {
        return entries_.at(entry_index).unit_values.contains(schema_.key_of(property));
    }

    [[nodiscard]] std::optional<int> int_value(size_t entry_index, iv::query::TagQueryPropertyId property) const override
    {
        auto const &values = entries_.at(entry_index).int_values;
        auto const it = values.find(schema_.key_of(property));
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] std::optional<float> float_value(size_t entry_index, iv::query::TagQueryPropertyId property) const override
    {
        auto const &values = entries_.at(entry_index).float_values;
        auto const it = values.find(schema_.key_of(property));
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

std::vector<size_t> execute(
    std::string_view source,
    iv::query::TagQuerySchema const &schema,
    iv::query::TagQueryDataset const &dataset)
{
    iv::query::TagQueryParser parser;
    auto const raw = parser.parse(source);
    auto const bound = iv::query::bind_tag_query_ast(raw, schema);
    return iv::query::execute_tag_query(bound, dataset).matching_entry_indexes;
}
} // namespace

TEST(TagQuery, ExistsMatchesTypedProperties)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"graph_input", iv::query::TagQueryValueType::unit},
        {"port_index", iv::query::TagQueryValueType::int_},
    });
    FakeTagQueryDataset dataset{
        schema,
        {
            {.unit_values = {{"graph_input", true}}, .int_values = {{"port_index", 1}}},
            {.unit_values = {}, .int_values = {}},
        }};

    EXPECT_EQ(execute("graph_input", schema, dataset), (std::vector<size_t>{0}));
    EXPECT_EQ(execute("port_index", schema, dataset), (std::vector<size_t>{0}));
}

TEST(TagQuery, AndOrAndNotCompose)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"graph_input", iv::query::TagQueryValueType::unit},
        {"knob", iv::query::TagQueryValueType::unit},
    });
    FakeTagQueryDataset dataset{
        schema,
        {
            {.unit_values = {{"graph_input", true}, {"knob", true}}},
            {.unit_values = {{"graph_input", true}}},
            {.unit_values = {{"knob", true}}},
        }};

    EXPECT_EQ(execute("graph_input knob", schema, dataset), (std::vector<size_t>{0}));
    EXPECT_EQ(execute("graph_input | knob", schema, dataset), (std::vector<size_t>{0, 1, 2}));
    EXPECT_EQ(execute("graph_input !knob", schema, dataset), (std::vector<size_t>{1}));
}

TEST(TagQuery, IntegerAndFloatRangesMatch)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"port_index", iv::query::TagQueryValueType::int_},
        {"gain", iv::query::TagQueryValueType::float_},
    });
    FakeTagQueryDataset dataset{
        schema,
        {
            {.int_values = {{"port_index", 1}}, .float_values = {{"gain", 0.25f}}},
            {.int_values = {{"port_index", 3}}, .float_values = {{"gain", 0.75f}}},
            {.int_values = {{"port_index", 5}}, .float_values = {{"gain", 1.25f}}},
        }};

    EXPECT_EQ(execute("port_index=2..5", schema, dataset), (std::vector<size_t>{1, 2}));
    EXPECT_EQ(execute("gain=0.5..1.0", schema, dataset), (std::vector<size_t>{1}));
}

TEST(TagQuery, GroupedPathExpansionWorks)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"tag.red", iv::query::TagQueryValueType::unit},
        {"tag.blue", iv::query::TagQueryValueType::unit},
        {"tag.green", iv::query::TagQueryValueType::unit},
    });
    FakeTagQueryDataset dataset{
        schema,
        {
            {.unit_values = {{"tag.red", true}, {"tag.blue", true}}},
            {.unit_values = {{"tag.green", true}}},
            {.unit_values = {{"tag.red", true}}},
        }};

    EXPECT_EQ(execute("tag.(red blue | green)", schema, dataset), (std::vector<size_t>{0, 1}));
}

TEST(TagQuery, IntegerProjectionMatchesProjectedValues)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"group", iv::query::TagQueryValueType::unit},
        {"port", iv::query::TagQueryValueType::int_},
        {"target", iv::query::TagQueryValueType::int_},
    });
    FakeTagQueryDataset dataset{
        schema,
        {
            {.unit_values = {{"group", true}}, .int_values = {{"port", 3}, {"target", 9}}},
            {.unit_values = {}, .int_values = {{"target", 3}}},
            {.unit_values = {}, .int_values = {{"target", 5}}},
        }};

    EXPECT_EQ(execute("target=group:port", schema, dataset), (std::vector<size_t>{1}));
}

TEST(TagQuery, UnitProjectionMatchesPresenceFromProjectedSubset)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"selected", iv::query::TagQueryValueType::unit},
        {"flag", iv::query::TagQueryValueType::unit},
    });
    FakeTagQueryDataset dataset{
        schema,
        {
            {.unit_values = {{"selected", true}, {"flag", true}}},
            {.unit_values = {{"selected", true}}},
            {.unit_values = {{"flag", true}}},
        }};

    EXPECT_EQ(execute("selected=flag:selected", schema, dataset), (std::vector<size_t>{0, 1}));
}

TEST(TagQuery, UnknownPropertyFailsBinding)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"graph_input", iv::query::TagQueryValueType::unit},
    });
    iv::query::TagQueryParser parser;
    auto const raw = parser.parse("missing");

    EXPECT_THROW(
        (void)iv::query::bind_tag_query_ast(raw, schema),
        std::runtime_error);
}

TEST(TagQuery, FilterPropertyMatchesMembership)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"graph_input", iv::query::TagQueryValueType::unit},
    });
    FakeTagQueryDataset dataset{
        schema,
        {
            {.filter_memberships = {{"graph_input.default", true}}},
            {.filter_memberships = {}},
        }};

    EXPECT_EQ(execute("filter.graph_input.default", schema, dataset), (std::vector<size_t>{0}));
}

TEST(TagQuery, FilterPropertyCanBeProjectedAsUnitValue)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"selected", iv::query::TagQueryValueType::unit},
    });
    FakeTagQueryDataset dataset{
        schema,
        {
            {.unit_values = {{"selected", true}}, .filter_memberships = {{"graph_input.default", true}}},
            {.unit_values = {{"selected", true}}, .filter_memberships = {}},
            {.unit_values = {}, .filter_memberships = {}},
        }};

    EXPECT_EQ(
        execute("selected=selected:filter.graph_input.default", schema, dataset),
        (std::vector<size_t>{0, 1}));
}

TEST(TagQueryCompletion, ReplacesTheCurrentPropertyAtom)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"dsp_graph.graph_input", iv::query::TagQueryValueType::unit},
        {"gain", iv::query::TagQueryValueType::float_},
    });

    auto const result = iv::query::complete_tag_query("dsp_gr", 6, schema);

    EXPECT_EQ(result.context, iv::query::TagQueryCompletionContext::property);
    EXPECT_EQ(result.replacement_range.start_offset, 0u);
    EXPECT_EQ(result.replacement_range.end_offset, 6u);
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].label, "dsp_graph.graph_input");
    EXPECT_EQ(result.candidates[0].insert_text, "dsp_graph.graph_input");
}

TEST(TagQueryCompletion, CompletesOnlyTheSegmentAfterADot)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"tag.blue", iv::query::TagQueryValueType::unit},
        {"tag.red", iv::query::TagQueryValueType::unit},
    });

    auto const result = iv::query::complete_tag_query("tag.r", 5, schema);

    EXPECT_EQ(result.context, iv::query::TagQueryCompletionContext::property);
    EXPECT_EQ(result.replacement_range.start_offset, 4u);
    EXPECT_EQ(result.replacement_range.end_offset, 5u);
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].label, "tag.red");
    EXPECT_EQ(result.candidates[0].insert_text, "red");
}

TEST(TagQueryCompletion, ConstrainsValueCandidatesByAssignedPropertyType)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"enabled", iv::query::TagQueryValueType::unit},
        {"gain", iv::query::TagQueryValueType::float_},
    });

    auto const unit_result = iv::query::complete_tag_query("enabled=", 8, schema);
    EXPECT_EQ(unit_result.context, iv::query::TagQueryCompletionContext::value);
    ASSERT_EQ(unit_result.candidates.size(), 1u);
    EXPECT_EQ(unit_result.candidates[0].insert_text, "unit");

    auto const numeric_result = iv::query::complete_tag_query("gain=", 5, schema);
    EXPECT_EQ(numeric_result.context, iv::query::TagQueryCompletionContext::value);
    ASSERT_EQ(numeric_result.candidates.size(), 2u);
    EXPECT_EQ(numeric_result.candidates[0].insert_text, "0");
    EXPECT_EQ(numeric_result.candidates[1].insert_text, "0..1");
}

TEST(TagQueryCompletion, CompletesProjectionSelectors)
{
    auto const schema = iv::query::TagQuerySchema::from_entries({
        {"group", iv::query::TagQueryValueType::unit},
        {"target", iv::query::TagQueryValueType::int_},
    });

    auto const result = iv::query::complete_tag_query("target=group:t", 14, schema);

    EXPECT_EQ(result.context, iv::query::TagQueryCompletionContext::projection_selector);
    EXPECT_EQ(result.replacement_range.start_offset, 13u);
    EXPECT_EQ(result.replacement_range.end_offset, 14u);
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].insert_text, "target");
}

TEST(TagQueryTokenizer, RetainsUtf8ByteRangesAndToleratesIncompleteEditorText)
{
    auto const tokens = iv::query::tokenize_tag_query("gain=1");
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].start_offset, 0u);
    EXPECT_EQ(tokens[0].end_offset, 4u);
    EXPECT_EQ(tokens[1].start_offset, 4u);
    EXPECT_EQ(tokens[1].end_offset, 5u);
    EXPECT_EQ(tokens[2].start_offset, 5u);
    EXPECT_EQ(tokens[2].end_offset, 6u);

    auto const incomplete = iv::query::tokenize_tag_query_tolerant("gain=-");
    ASSERT_EQ(incomplete.size(), 4u);
    EXPECT_EQ(incomplete[2].kind, iv::query::TagQueryToken::Kind::invalid);
    EXPECT_EQ(incomplete[2].start_offset, 5u);
    EXPECT_EQ(incomplete[2].end_offset, 6u);
}

TEST(NodeDefinitionSearch, ExposesMetadataAndUsesTagQueriesForDefinitions)
{
    using Json = nlohmann::ordered_json;
    iv::NodeDefinitionsSnapshot catalog{.generation = 7};
    auto add = [&](std::string id, std::string name, std::vector<std::string> tags) {
        catalog.by_id.emplace(id, iv::NodeDefinitionEntry{.definition_id = id,
            .definition = iv::LeafNodeDefinition{.definition_id = id, .display_name = std::move(name),
                .presentation = {.tags = std::move(tags), .ports = {{"value", false, true}}}}});
    };
    add("iv.project.q24_pan.drive", "Drive", {"effect.saturation"});
    add("iv.project.q24_pan.other", "Other", {"effect.saturation", "experimental"});
    add("iv.project.q24_panner", "Panner", {});

    auto const definitions = iv::search_node_definitions(catalog, "", std::nullopt);
    ASSERT_EQ(definitions.at("definitions").size(), 3u);
    auto const& drive = definitions.at("definitions").front();
    EXPECT_EQ(drive.at("displayName"), "Drive");
    EXPECT_EQ(drive.at("ports").front().at("name"), "value");
    EXPECT_EQ(definitions.at("generation"), "7");

    auto const at_start = iv::search_node_definitions(catalog, "", 0).at("completion");
    EXPECT_EQ(at_start.at("start"), 0);
    EXPECT_EQ(at_start.at("end"), 0);
    EXPECT_TRUE(std::ranges::any_of(at_start.at("candidates"),
        [](auto const& candidate) { return candidate.at("insertText") == "effect.saturation"; }));

    auto const scope = iv::search_node_definitions(catalog, "id.iv.project.q24_pan", 21);
    EXPECT_EQ(scope.at("matches"), Json::array({"iv.project.q24_pan.drive", "iv.project.q24_pan.other"}));
    auto const intersection = iv::search_node_definitions(catalog, "effect.saturation !experimental", 31);
    EXPECT_EQ(intersection.at("matches"), Json::array({"iv.project.q24_pan.drive"}));
    auto const completed = iv::search_node_definitions(catalog, "effect.sat !experimental", 10).at("completion");
    EXPECT_EQ(completed.at("start"), 7);
    EXPECT_EQ(completed.at("end"), 10);
    ASSERT_EQ(completed.at("candidates").size(), 1u);
    EXPECT_EQ(completed.at("candidates").front().at("insertText"), "saturation");
}
