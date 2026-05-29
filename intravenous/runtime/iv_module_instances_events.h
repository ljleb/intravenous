#pragma once

#include "runtime/iv_module_instance_types.h"
#include "linker_event.h"

#include <filesystem>
#include <string>
#include <vector>

namespace iv {
struct IvModuleRequiredDefinitionsChanged;
using IvModuleRequiredDefinitionsChangedEvent =
    void (*)(IvModuleRequiredDefinitionsChanged const &);
struct IvModuleInstancesChanged;
using IvModuleInstancesChangedEvent =
    void (*)(IvModuleInstancesChanged const &);
struct IvModuleInstanceBuildersChanged;
using IvModuleInstanceBuildersChangedEvent =
    void (*)(IvModuleInstanceBuildersChanged const &);
using IvModuleInstancesListChangedEvent =
    void (*)(std::vector<IvModuleInstanceInfo> const &);

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
    IvModuleInstancesListChangedEvent,
    iv_runtime_iv_module_instances_list_changed_event);
} // namespace iv
