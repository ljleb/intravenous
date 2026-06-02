#pragma once

#include "linker_event.h"
#include "runtime/iv_module_definitions.h"

namespace iv {
using IvModuleDefinitionDeclarationsChangedEvent =
    void (*)(IvModuleDefinitionDeclarationsChanged const &);
using IvModuleDefinitionsChangedEvent =
    void (*)(IvModuleDefinitionsChanged const &);
using IvModuleDefinitionsNotificationEvent =
    void (*)(IvModuleDefinitionsNotification const &);

IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionDeclarationsChangedEvent,
    iv_runtime_iv_module_definitions_declarations_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionsNotificationEvent,
    iv_runtime_iv_module_definitions_notification_event);
} // namespace iv
