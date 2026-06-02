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
enum class LaneQueryValueType : std::uint8_t {
    unit,
    int_,
    float_,
};

struct LaneQueryPropertyId {
    std::uint32_t value = 0;

    auto operator<=>(LaneQueryPropertyId const &) const = default;
};

struct LaneQuerySchemaEntry {
    std::string key {};
    LaneQueryPropertyId id {};
    LaneQueryValueType type = LaneQueryValueType::unit;
};

struct LaneQuerySchemaChange {
    struct Added {
        LaneQuerySchemaEntry entry {};
    };

    struct Removed {
        LaneQuerySchemaEntry entry {};
    };

    struct Retyped {
        std::string key {};
        LaneQueryPropertyId id {};
        LaneQueryValueType old_type = LaneQueryValueType::unit;
        LaneQueryValueType new_type = LaneQueryValueType::unit;
    };

    bool changed = false;
    std::uint64_t old_revision = 0;
    std::uint64_t new_revision = 0;
    std::vector<Added> added {};
    std::vector<Removed> removed {};
    std::vector<Retyped> retyped {};
};

class LaneQuerySchema {
    std::vector<LaneQuerySchemaEntry> _entries;
    std::unordered_map<std::string, LaneQueryPropertyId> _id_by_key;
    std::uint64_t _revision = 0;

public:
    LaneQuerySchema() = default;

    static LaneQuerySchema from_entries(
        std::vector<std::pair<std::string, LaneQueryValueType>> entries,
        std::uint64_t revision = 0)
    {
        std::sort(entries.begin(), entries.end(), [](auto const &lhs, auto const &rhs) {
            return lhs.first < rhs.first;
        });

        LaneQuerySchema schema;
        schema._revision = revision;
        schema._entries.reserve(entries.size());
        for (std::uint32_t index = 0; index < entries.size(); ++index) {
            auto &[key, type] = entries[index];
            if (schema._id_by_key.contains(key)) {
                throw std::runtime_error("duplicate lane query schema key: " + key);
            }
            LaneQueryPropertyId const id{index};
            schema._entries.push_back(LaneQuerySchemaEntry{
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

    [[nodiscard]] std::span<LaneQuerySchemaEntry const> entries() const noexcept
    {
        return _entries;
    }

    [[nodiscard]] std::optional<LaneQueryPropertyId> find(std::string_view key) const
    {
        auto const it = _id_by_key.find(std::string(key));
        if (it == _id_by_key.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] LaneQueryValueType type_of(LaneQueryPropertyId id) const
    {
        if (id.value >= _entries.size()) {
            throw std::runtime_error("unknown lane query property id");
        }
        return _entries[id.value].type;
    }

    [[nodiscard]] std::string const &key_of(LaneQueryPropertyId id) const
    {
        if (id.value >= _entries.size()) {
            throw std::runtime_error("unknown lane query property id");
        }
        return _entries[id.value].key;
    }
};

inline LaneQuerySchemaChange diff_lane_query_schemas(
    LaneQuerySchema const &old_schema,
    LaneQuerySchema const &new_schema)
{
    LaneQuerySchemaChange diff{
        .old_revision = old_schema.revision(),
        .new_revision = new_schema.revision(),
    };

    std::unordered_map<std::string, LaneQuerySchemaEntry> old_by_key;
    for (auto const &entry : old_schema.entries()) {
        old_by_key.emplace(entry.key, entry);
    }

    std::unordered_map<std::string, LaneQuerySchemaEntry> new_by_key;
    for (auto const &entry : new_schema.entries()) {
        new_by_key.emplace(entry.key, entry);
    }

    for (auto const &[key, old_entry] : old_by_key) {
        auto const new_it = new_by_key.find(key);
        if (new_it == new_by_key.end()) {
            diff.changed = true;
            diff.removed.push_back(LaneQuerySchemaChange::Removed{.entry = old_entry});
            continue;
        }
        if (new_it->second.type != old_entry.type) {
            diff.changed = true;
            diff.retyped.push_back(LaneQuerySchemaChange::Retyped{
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
            diff.added.push_back(LaneQuerySchemaChange::Added{.entry = new_entry});
        }
    }

    return diff;
}
} // namespace iv::query
