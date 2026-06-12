#pragma once

#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/linker_event.h>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
struct IvModuleRequiredDefinitionsChanged;
using IvModuleRequiredDefinitionsChangedEvent =
    void (*)(IvModuleRequiredDefinitionsChanged const &);
struct IvModuleInstancesChanged;
using IvModuleInstancesChangedEvent =
    void (*)(IvModuleInstancesChanged const &);
struct IvModuleInstanceBuildersChanged;
class IvModuleInstanceBuildersAckBuilder;
using IvModuleInstanceBuildersChangedEvent =
    void (*)(IvModuleInstanceBuildersChanged const &, IvModuleInstanceBuildersAckBuilder &);
using IvModuleInstanceBuildersCompletedEvent =
    void (*)(IvModuleInstanceBuildersChanged const &);
using IvModuleInstancesListChangedEvent =
    void (*)(std::vector<IvModuleInstanceInfo> const &);

class IvModuleInstanceBuildersAckBuilder {
    std::unordered_map<std::string, std::vector<LaneId>> prerequisite_lanes_by_instance_id_;

public:
    void set_prerequisite_lanes(
        std::string instance_id,
        std::vector<LaneId> prerequisite_lanes);
    [[nodiscard]] std::optional<std::vector<LaneId>> prerequisite_lanes_for(
        std::string const &instance_id) const;
};

IV_DECLARE_LINKER_EVENT(
    IvModuleRequiredDefinitionsChangedEvent,
    iv_runtime_iv_module_required_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleInstancesChangedEvent,
    iv_runtime_iv_module_instances_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleInstanceBuildersChangedEvent,
    iv_runtime_iv_module_instance_builders_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleInstanceBuildersCompletedEvent,
    iv_runtime_iv_module_instance_builders_completed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleInstancesListChangedEvent,
    iv_runtime_iv_module_instances_list_changed_event);
} // namespace iv
