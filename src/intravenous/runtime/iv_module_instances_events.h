#pragma once

#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/linker_event.h>

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
struct IvModuleDefinitionsChanged;
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

struct IvModuleInstancesConfigured {
    IvModuleDefinitionsChanged const* definitions = nullptr;
    IvModuleInstanceBuildersChanged const* builders = nullptr;
    std::optional<GraphInputPublicPortsSnapshot> public_ports{};
};

using IvModuleInstancesConfiguredEvent =
    void (*)(IvModuleInstancesConfigured const &);
using IvModuleInstancesListChangedEvent =
    void (*)(std::vector<IvModuleInstanceInfo> const &);

class IvModuleInstanceBuildersAckBuilder {
    std::unordered_map<std::string, std::vector<LaneId>> prerequisite_lanes_by_instance_id_;
    std::optional<std::uint64_t> version_index_;
    std::optional<GraphInputPublicPortsSnapshot> public_ports_;

public:
    void set_prerequisite_lanes(
        std::string instance_id,
        std::vector<LaneId> prerequisite_lanes);
    void set_version_index(std::uint64_t version_index);
    void set_public_ports(GraphInputPublicPortsSnapshot public_ports);
    [[nodiscard]] std::optional<std::vector<LaneId>> prerequisite_lanes_for(
        std::string const &instance_id) const;
    [[nodiscard]] std::optional<std::uint64_t> version_index() const;
    [[nodiscard]] std::optional<GraphInputPublicPortsSnapshot> take_public_ports();
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
    IvModuleInstancesConfiguredEvent,
    iv_runtime_iv_module_instances_configured_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleInstancesListChangedEvent,
    iv_runtime_iv_module_instances_list_changed_event);
} // namespace iv
