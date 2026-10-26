#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_module_instance_types.h>

#include <vector>

namespace iv {
struct IvModuleRequiredDefinitionsChanged;
using IvModuleRequiredDefinitionsChangedEvent =
    void (*)(IvModuleRequiredDefinitionsChanged const &);
struct IvModuleInstancesChanged;
using IvModuleInstancesChangedEvent = void (*)(IvModuleInstancesChanged const &);
using IvModuleInstancesListChangedEvent =
    void (*)(std::vector<IvModuleInstanceInfo> const &);
// Project-owned instance declarations only. Definition realization changes use
// IvModuleInstancesListChangedEvent and must not feed back into definition-owned
// source introspection through this event.
using IvModuleInstanceDeclarationsChangedEvent =
    void (*)(std::vector<IvModuleInstanceInfo> const &);

IV_DECLARE_LINKER_EVENT(
    IvModuleRequiredDefinitionsChangedEvent,
    iv_runtime_iv_module_required_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleInstancesChangedEvent,
    iv_runtime_iv_module_instances_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleInstancesListChangedEvent,
    iv_runtime_iv_module_instances_list_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleInstanceDeclarationsChangedEvent,
    iv_runtime_iv_module_instance_declarations_changed_event);
} // namespace iv
