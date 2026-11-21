#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_module_instance_types.h>

#include <vector>

namespace iv {
struct IvModuleRequiredDefinitionsChanged;
using IvModuleRequiredDefinitionsChangedEvent =
    void (*)(IvModuleRequiredDefinitionsChanged const &);

// NodeInstances raises no presentation notifications: its instance-list and
// declaration changes return to ProjectGraph in NodeInstancesProjectGraphRequest.
IV_DECLARE_LINKER_EVENT(
    IvModuleRequiredDefinitionsChangedEvent,
    iv_runtime_iv_module_required_definitions_changed_event);
} // namespace iv
