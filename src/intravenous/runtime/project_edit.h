#pragma once

#include <intravenous/runtime/project_settings.h>

#include <intravenous/runtime/project_node_instance_types.h>
#include <intravenous/runtime/project_node_replacement_types.h>
#include <intravenous/runtime/project_connection_types.h>
#include <intravenous/node/manual_control.h>

#include <algorithm>
#include <concepts>
#include <optional>
#include <stdexcept>
#include <vector>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace iv {
inline auto const& project_record_key(ProjectNodeInstanceDeclaration const& value) { return value.instance_id; }
inline auto const& project_record_key(ProjectNodeReplacement const& value) { return value.address; }
inline auto const& project_record_key(ProjectConnection const& value) { return project_connection_id(value); }
inline auto const& project_record_key(ProjectManualControl const& value) { return value.id; }

template<class Record, class Key>
struct ProjectRecordEdit {
    Key key{};
    // Absent means remove the record; an upsert supplies its complete value.
    std::optional<Record> value{};
    bool require_existing = false;
};

struct ProjectInstanceUpdate {
    std::string instance_id{};
    std::optional<std::string> display_name{};
    std::optional<std::string> configuration_source{};
};
struct ProjectEditBatch {
    // Existing create/update RPC semantics, resolved inside the same batch.
    std::vector<std::string> require_absent_instances{};
    std::vector<ProjectInstanceUpdate> instance_updates{};
    std::vector<ProjectRecordEdit<ProjectNodeInstanceDeclaration, std::string>> instances{};
    std::vector<ProjectRecordEdit<ProjectNodeReplacement, NodeAddress>> replacements{};
    std::vector<ProjectRecordEdit<ProjectConnection, std::string>> connections{};
    std::vector<ProjectRecordEdit<ProjectManualControl, ManualControlId>> manual_controls{};
    std::optional<ProjectAudioDeviceSelection> audio_devices{};
    std::optional<ProjectToolchainSettings> toolchain{};
};

// Temporary owner-provided snapshots, never a second canonical project model.
struct ProjectInstanceRecords {
    std::vector<ProjectNodeInstanceDeclaration> instances{};
    std::vector<ProjectNodeReplacement> replacements{};
};

template<class Record>
struct ProjectRecordChange {
    std::optional<Record> before{};
    std::optional<Record> after{};
};
struct ProjectEditEffect {
    std::vector<ProjectRecordChange<ProjectNodeInstanceDeclaration>> instances{};
    std::vector<ProjectRecordChange<ProjectNodeReplacement>> replacements{};
    std::vector<ProjectRecordChange<ProjectConnection>> connections{};
    std::vector<ProjectRecordChange<ProjectManualControl>> manual_controls{};
    std::optional<ProjectRecordChange<ProjectAudioDeviceSelection>> audio_devices{};
    std::optional<ProjectRecordChange<ProjectToolchainSettings>> toolchain{};
    [[nodiscard]] bool empty() const {
        return instances.empty() && replacements.empty() && connections.empty() && manual_controls.empty()
            && !audio_devices && !toolchain;
    }
};

struct ProjectRecordKeyHash {
    std::size_t operator()(std::string const& value) const { return std::hash<std::string>{}(value); }
    std::size_t operator()(NodeAddress const& value) const {
        auto hash = (*this)(value.instance_id);
        auto mix = [&](std::size_t next) { hash ^= next + 0x9e3779b9 + (hash << 6) + (hash >> 2); };
        for (auto const& selector : value.path) {
            mix(selector.index());
            std::visit([&](auto const& part) {
                using Part = std::remove_cvref_t<decltype(part)>;
                if constexpr (std::same_as<Part, ProjectVirtualNodeSelector>) {
                    mix((*this)(part.source_identity));
                    mix(part.type_identity ? (*this)(*part.type_identity) : 0);
                } else if constexpr (std::same_as<Part, ProjectVirtualMemberSelector>) mix(part.index);
                else mix((*this)(part.name));
            }, selector);
        }
        return hash;
    }
    std::size_t operator()(ManualControlId const& value) const {
        return (*this)(value.node) ^ ((*this)(value.input_name) << 1);
    }
};

template<class Record, class Key>
void apply_project_record_edits(std::vector<Record>& records,
    std::vector<ProjectRecordEdit<Record, Key>> const& edits)
{
    if (edits.empty()) return;
    std::unordered_map<Key, std::size_t, ProjectRecordKeyHash> indices;
    for (std::size_t index = 0; index < records.size(); ++index) indices.emplace(project_record_key(records[index]), index);
    std::unordered_set<Key, ProjectRecordKeyHash> seen;
    std::vector<bool> removed(records.size(), false);
    for (auto const& edit : edits) {
        if (!seen.insert(edit.key).second)
            throw std::invalid_argument("project batch edits the same record twice");
        if (edit.value && project_record_key(*edit.value) != edit.key)
            throw std::invalid_argument("project batch record identity does not match its key");
        auto found = indices.find(edit.key);
        if (edit.require_existing && found == indices.end())
            throw std::invalid_argument("project batch addresses an unknown record");
        if (edit.value) {
            if (found == indices.end()) {
                indices.emplace(edit.key, records.size());
                records.push_back(*edit.value);
                removed.push_back(false);
            } else records[found->second] = *edit.value;
        } else if (found != indices.end()) removed[found->second] = true;
    }
    std::vector<Record> retained;
    retained.reserve(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (!removed[index]) retained.push_back(std::move(records[index]));
    }
    records = std::move(retained);
}

template<class Record>
std::vector<ProjectRecordChange<Record>> project_record_changes(
    std::vector<Record> const& before, std::vector<Record> const& after)
{
    using Key = std::remove_cvref_t<decltype(project_record_key(std::declval<Record const&>()))>;
    std::unordered_map<Key, Record const*, ProjectRecordKeyHash> remaining;
    remaining.reserve(after.size());
    for (auto const& value : after) remaining.emplace(project_record_key(value), &value);
    std::vector<ProjectRecordChange<Record>> changes;
    for (auto const& value : before) {
        auto found = remaining.find(project_record_key(value));
        if (found == remaining.end()) changes.push_back({value, std::nullopt});
        else {
            if (*found->second != value) changes.push_back({value, *found->second});
            remaining.erase(found);
        }
    }
    for (auto const& value : after) {
        if (remaining.contains(project_record_key(value))) changes.push_back({std::nullopt, value});
    }
    return changes;
}

template<class Record, class Key>
std::vector<ProjectRecordEdit<Record, Key>> project_history_edits(
    std::vector<Record> const& current, std::vector<ProjectRecordChange<Record>> const& changes, bool undo)
{
    std::unordered_map<Key, Record const*, ProjectRecordKeyHash> records;
    for (auto const& value : current) records.emplace(project_record_key(value), &value);
    std::vector<ProjectRecordEdit<Record, Key>> edits;
    for (auto const& change : changes) {
        auto const& expected = undo ? change.after : change.before;
        auto const& desired = undo ? change.before : change.after;
        auto const& key = project_record_key(expected ? *expected : *desired);
        auto found = records.find(key);
        if (expected ? found == records.end() || *found->second != *expected : found != records.end())
            throw std::runtime_error("project history no longer matches the accepted record");
        edits.push_back({key, desired});
    }
    return edits;
}
template<class Record>
Record project_history_setting(Record const& current, ProjectRecordChange<Record> const& change, bool undo)
{
    auto const& expected = undo ? change.after : change.before;
    auto const& desired = undo ? change.before : change.after;
    if (!expected || !desired || current != *expected)
        throw std::runtime_error("project history no longer matches the accepted setting");
    return *desired;
}

} // namespace iv
