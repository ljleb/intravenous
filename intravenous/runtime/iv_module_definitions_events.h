#pragma once

#include "linker_event.h"
#include "runtime/iv_module_definitions.h"

namespace iv {
using RuntimeIvModuleDefinitionDeclarationsChangedEvent =
    void (*)(RuntimeIvModuleDefinitionDeclarationsChanged const &);
using RuntimeIvModuleDefinitionsChangedEvent =
    void (*)(RuntimeIvModuleDefinitionsChanged const &);
using RuntimeIvModuleDefinitionsNotificationEvent =
    void (*)(RuntimeIvModuleDefinitionsNotification const &);

IV_DECLARE_LINKER_EVENT(
    RuntimeIvModuleDefinitionDeclarationsChangedEvent,
    iv_runtime_iv_module_definitions_declarations_changed_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeIvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeIvModuleDefinitionsNotificationEvent,
    iv_runtime_iv_module_definitions_notification_event);
} // namespace iv
