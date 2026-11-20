#include <intravenous/runtime/node_definition_search.h>

#include <intravenous/query/tag_query_completion.h>
#include <intravenous/query/tag_query_engine.h>
#include <intravenous/query/tag_query_parser.h>
#include <intravenous/runtime/node_definition_types.h>

#include <nlohmann/json.hpp>

#include <set>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;

class DefinitionTagDataset final : public query::TagQueryDataset {
    query::TagQuerySchema schema_;
    std::vector<std::set<std::string, std::less<>>> tags_;

public:
    explicit DefinitionTagDataset(std::span<NodeDefinitionEntry const* const> definitions,
        std::uint64_t generation)
    {
        std::set<std::string> keys;
        tags_.reserve(definitions.size());
        for (auto const* entry : definitions) {
            auto& tags = tags_.emplace_back();
            std::visit([&](auto const& definition) {
                tags.insert(definition.presentation.tags.begin(), definition.presentation.tags.end());
            }, entry->definition);
            auto const id = "id." + entry->definition_id;
            tags.insert(id);
            for (auto end = id.rfind('.'); end != std::string::npos && end > 2; end = id.rfind('.', end - 1))
                tags.insert(id.substr(0, end));
            keys.insert(tags.begin(), tags.end());
        }
        std::vector<std::pair<std::string, query::TagQueryValueType>> properties;
        properties.reserve(keys.size());
        for (auto const& key : keys) properties.emplace_back(key, query::TagQueryValueType::unit);
        schema_ = query::TagQuerySchema::from_entries(std::move(properties), generation);
    }

    query::TagQuerySchema const& schema() const override { return schema_; }
    std::size_t entry_count() const override { return tags_.size(); }
    bool in_filter(std::size_t, std::string_view) const override { return false; }
    bool has_unit(std::size_t index, query::TagQueryPropertyId key) const override
    {
        return tags_[index].contains(schema_.key_of(key));
    }
    std::optional<int> int_value(std::size_t, query::TagQueryPropertyId) const override { return std::nullopt; }
    std::optional<float> float_value(std::size_t, query::TagQueryPropertyId) const override { return std::nullopt; }
};
} // namespace

nlohmann::ordered_json search_node_definitions(NodeDefinitionsSnapshot const& snapshot,
    std::string_view source, std::optional<std::size_t> cursor)
{
    std::vector<NodeDefinitionEntry const*> definitions;
    definitions.reserve(snapshot.by_id.size());
    for (auto const& [_, definition] : snapshot.by_id) definitions.push_back(&definition);
    std::ranges::sort(definitions, {}, &NodeDefinitionEntry::definition_id);
    Json result{{"generation", std::to_string(snapshot.generation)}};
    if (!cursor) {
        auto& catalog = result["definitions"] = Json::array();
        for (auto const* entry : definitions) {
            std::visit([&](auto const& definition) {
                auto ports = Json::array();
                for (auto const& port : definition.presentation.ports) {
                    ports.push_back({{"name", port.name}, {"event", port.event}, {"output", port.output}});
                }
                catalog.push_back({{"id", entry->definition_id}, {"displayName", definition.display_name},
                    {"package", definition.package_root.filename().string()}, {"tags", definition.presentation.tags},
                    {"ports", std::move(ports)}, {"requiresConfiguration", definition.presentation.required_arguments != 0}});
            }, entry->definition);
        }
        return result;
    }

    DefinitionTagDataset dataset{definitions, snapshot.generation};
    auto const completion = query::complete_tag_query(source, *cursor, dataset.schema());
    auto candidates = Json::array();
    for (auto const& candidate : completion.candidates) {
        candidates.push_back({{"label", candidate.label}, {"insertText", candidate.insert_text}});
    }
    auto diagnostics = Json::array();
    for (auto const& diagnostic : completion.diagnostics) {
        diagnostics.push_back({{"message", diagnostic.message}, {"start", diagnostic.range.start_offset},
            {"end", diagnostic.range.end_offset}});
    }
    result["completion"] = {{"start", completion.replacement_range.start_offset},
        {"end", completion.replacement_range.end_offset}, {"candidates", std::move(candidates)},
        {"diagnostics", std::move(diagnostics)}};
    if (source.empty()) return result;
    try {
        auto const parsed = query::TagQueryParser{}.parse(source);
        auto const bound = query::bind_tag_query_ast(parsed, dataset.schema());
        auto const matches = query::execute_tag_query(bound, dataset);
        auto& ids = result["matches"] = Json::array();
        for (auto index : matches.matching_entry_indexes) ids.push_back(definitions[index]->definition_id);
    } catch (std::runtime_error const& error) {
        result["error"] = error.what();
    }
    return result;
}
} // namespace iv
