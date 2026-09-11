#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_module_definitions.h>


namespace iv {
using IvPackageDeclarationsChangedEvent =
    void (*)(IvPackageDeclarationsChanged const &);
using IvPackageDefinitionsChangedEvent =
    void (*)(IvPackageDefinitionsChanged const &);
using IvModuleDefinitionsNotificationEvent =
    void (*)(IvModuleDefinitionsNotification const &);

IV_DECLARE_LINKER_EVENT(
    IvPackageDeclarationsChangedEvent,
    iv_runtime_iv_package_declarations_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvPackageDefinitionsChangedEvent,
    iv_runtime_iv_package_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionsNotificationEvent,
    iv_runtime_iv_module_definitions_notification_event);
} // namespace iv
