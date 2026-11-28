#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iv::query {
enum class TagQueryValueType : std::uint8_t {
    unit,
    int_,
    float_,
};

struct TagQueryPropertyId {
    std::uint32_t value = 0;

    auto operator<=>(TagQueryPropertyId const &) const = default;
};

struct TagQuerySchemaEntry {
    std::string key {};
    TagQueryPropertyId id {};
    TagQueryValueType type = TagQueryValueType::unit;
};

struct TagQuerySchemaChange {
    struct Added {
        TagQuerySchemaEntry entry {};
    };

    struct Removed {
        TagQuerySchemaEntry entry {};
    };

    struct Retyped {
        std::string key {};
        TagQueryPropertyId id {};
        TagQueryValueType old_type = TagQueryValueType::unit;
        TagQueryValueType new_type = TagQueryValueType::unit;
    };

    bool changed = false;
    std::uint64_t old_revision = 0;
    std::uint64_t new_revision = 0;
    std::vector<Added> added {};
    std::vector<Removed> removed {};
    std::vector<Retyped> retyped {};
};

class TagQuerySchema {
    std::vector<TagQuerySchemaEntry> _entries;
    std::unordered_map<std::string, TagQueryPropertyId> _id_by_key;
    std::uint64_t _revision = 0;

public:
    TagQuerySchema() = default;

    static TagQuerySchema from_entries(
        std::vector<std::pair<std::string, TagQueryValueType>> entries,
        std::uint64_t revision = 0)
    {
        std::sort(entries.begin(), entries.end(), [](auto const &lhs, auto const &rhs) {
            return lhs.first < rhs.first;
        });

        TagQuerySchema schema;
        schema._revision = revision;
        schema._entries.reserve(entries.size());
        for (std::uint32_t index = 0; index < entries.size(); ++index) {
            auto &[key, type] = entries[index];
            if (schema._id_by_key.contains(key)) {
                throw std::runtime_error("duplicate tag query schema key: " + key);
            }
            TagQueryPropertyId const id{index};
            schema._entries.push_back(TagQuerySchemaEntry{
                .key = std::move(key),
                .id = id,
                .type = type,
            });
            schema._id_by_key.emplace(schema._entries.back().key, id);
        }
        return schema;
    }

    [[nodiscard]] std::uint64_t revision() const noexcept
    {
        return _revision;
    }

    [[nodiscard]] std::span<TagQuerySchemaEntry const> entries() const noexcept
    {
        return _entries;
    }

    [[nodiscard]] std::optional<TagQueryPropertyId> find(std::string_view key) const
    {
        auto const it = _id_by_key.find(std::string(key));
        if (it == _id_by_key.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] TagQueryValueType type_of(TagQueryPropertyId id) const
    {
        if (id.value >= _entries.size()) {
            throw std::runtime_error("unknown tag query property id");
        }
        return _entries[id.value].type;
    }

    [[nodiscard]] std::string const &key_of(TagQueryPropertyId id) const
    {
        if (id.value >= _entries.size()) {
            throw std::runtime_error("unknown tag query property id");
        }
        return _entries[id.value].key;
    }
};

inline TagQuerySchemaChange diff_tag_query_schemas(
    TagQuerySchema const &old_schema,
    TagQuerySchema const &new_schema)
{
    TagQuerySchemaChange diff{
        .old_revision = old_schema.revision(),
        .new_revision = new_schema.revision(),
    };

    std::unordered_map<std::string, TagQuerySchemaEntry> old_by_key;
    for (auto const &entry : old_schema.entries()) {
        old_by_key.emplace(entry.key, entry);
    }

    std::unordered_map<std::string, TagQuerySchemaEntry> new_by_key;
    for (auto const &entry : new_schema.entries()) {
        new_by_key.emplace(entry.key, entry);
    }

    for (auto const &[key, old_entry] : old_by_key) {
        auto const new_it = new_by_key.find(key);
        if (new_it == new_by_key.end()) {
            diff.changed = true;
            diff.removed.push_back(TagQuerySchemaChange::Removed{.entry = old_entry});
            continue;
        }
        if (new_it->second.type != old_entry.type) {
            diff.changed = true;
            diff.retyped.push_back(TagQuerySchemaChange::Retyped{
                .key = key,
                .id = new_it->second.id,
                .old_type = old_entry.type,
                .new_type = new_it->second.type,
            });
        }
    }

    for (auto const &[key, new_entry] : new_by_key) {
        if (!old_by_key.contains(key)) {
            diff.changed = true;
            diff.added.push_back(TagQuerySchemaChange::Added{.entry = new_entry});
        }
    }

    return diff;
}
} // namespace iv::query
