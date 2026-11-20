#pragma once

#include <intravenous/runtime/project_settings.h>

#include <intravenous/runtime/project_node_instance_types.h>
#include <intravenous/runtime/project_node_replacement_types.h>
#include <intravenous/runtime/project_connection_types.h>
#include <intravenous/runtime/project_presentation.h>
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
inline auto const& project_record_key(ProjectLaneView const& value) { return value.id; }

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
struct ProjectInstanceDuplicate {
    std::string source_instance_id{};
    std::string instance_id{};
};
struct ProjectEditBatch {
    // Existing create/update RPC semantics, resolved inside the same batch.
    std::vector<std::string> require_absent_instances{};
    std::vector<ProjectInstanceUpdate> instance_updates{};
    std::vector<ProjectInstanceDuplicate> instance_duplicates{};
    std::vector<ProjectRecordEdit<ProjectNodeInstanceDeclaration, std::string>> instances{};
    std::vector<ProjectRecordEdit<ProjectNodeReplacement, NodeAddress>> replacements{};
    std::vector<ProjectRecordEdit<ProjectConnection, std::string>> connections{};
    std::vector<ProjectRecordEdit<ProjectManualControl, ManualControlId>> manual_controls{};
    std::optional<ProjectAudioDeviceSelection> audio_devices{};
    std::optional<ProjectToolchainSettings> toolchain{};
    std::vector<ProjectRecordEdit<ProjectLaneView, ProjectLaneViewId>> lane_views{};
    std::optional<std::vector<NodeAddress>> lane_order{};
    std::optional<nlohmann::json> timeline_snap_memory{};
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
    std::vector<ProjectRecordChange<ProjectLaneView>> lane_views{};
    std::optional<ProjectRecordChange<std::vector<NodeAddress>>> lane_order{};
    std::optional<ProjectRecordChange<nlohmann::json>> timeline_snap_memory{};
    [[nodiscard]] bool empty() const {
        return instances.empty() && replacements.empty() && connections.empty() && manual_controls.empty()
            && !audio_devices && !toolchain && lane_views.empty() && !lane_order && !timeline_snap_memory;
    }
};

template<class Record>
bool project_changes_overlap(std::vector<ProjectRecordChange<Record>> const& left,
    std::vector<ProjectRecordChange<Record>> const& right)
{
    return std::ranges::any_of(left, [&](auto const& a) {
        auto const& key = project_record_key(a.after ? *a.after : *a.before);
        return std::ranges::any_of(right, [&](auto const& b) {
            return key == project_record_key(b.after ? *b.after : *b.before);
        });
    });
}

inline bool project_effects_overlap(ProjectEditEffect const& left, ProjectEditEffect const& right)
{
    return project_changes_overlap(left.instances, right.instances)
        || project_changes_overlap(left.replacements, right.replacements)
        || project_changes_overlap(left.connections, right.connections)
        || project_changes_overlap(left.manual_controls, right.manual_controls)
        || project_changes_overlap(left.lane_views, right.lane_views)
        || (left.lane_order && right.lane_order) || (left.timeline_snap_memory && right.timeline_snap_memory)
        || (left.audio_devices && right.audio_devices) || (left.toolchain && right.toolchain);
}

template<class Record>
void merge_project_changes(std::vector<ProjectRecordChange<Record>>& into,
    std::vector<ProjectRecordChange<Record>> changes)
{
    for (auto& change : changes) {
        auto const& key = project_record_key(change.after ? *change.after : *change.before);
        auto existing = std::ranges::find_if(into, [&](auto const& value) {
            return key == project_record_key(value.after ? *value.after : *value.before);
        });
        if (existing == into.end()) into.push_back(std::move(change));
        else existing->after = std::move(change.after);
    }
    std::erase_if(into, [](auto const& change) { return change.before == change.after; });
}

inline void merge_project_effect(ProjectEditEffect& into, ProjectEditEffect effect)
{
    merge_project_changes(into.instances, std::move(effect.instances));
    merge_project_changes(into.replacements, std::move(effect.replacements));
    merge_project_changes(into.connections, std::move(effect.connections));
    merge_project_changes(into.manual_controls, std::move(effect.manual_controls));
    merge_project_changes(into.lane_views, std::move(effect.lane_views));
    auto merge_setting = [](auto& target, auto source) {
        if (!source) return;
        if (target) target->after = std::move(source->after);
        else target = std::move(source);
        if (target->before == target->after) target.reset();
    };
    merge_setting(into.audio_devices, std::move(effect.audio_devices));
    merge_setting(into.toolchain, std::move(effect.toolchain));
    merge_setting(into.lane_order, std::move(effect.lane_order));
    merge_setting(into.timeline_snap_memory, std::move(effect.timeline_snap_memory));
}

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
        return (*this)(value.node) ^ ((*this)(value.port_name) << 1)
            ^ (static_cast<std::size_t>(value.kind) << 2);
    }
    std::size_t operator()(ProjectLaneViewId const& value) const {
        return (*this)(value.address) ^ ((*this)(value.presentation_id) << 1);
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
