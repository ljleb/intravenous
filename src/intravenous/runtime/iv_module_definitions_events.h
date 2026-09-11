#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_module_definitions.h>


namespace iv {
using IvPackageDeclarationsChangedEvent =
    void (*)(IvPackageDeclarationsChanged const &);
using IvModuleDefinitionsChangedEvent =
    void (*)(IvModuleDefinitionsChanged const &);
using IvNodeTypeDefinitionsChangedEvent =
    void (*)(IvNodeTypeDefinitionsChanged const &);
using IvModuleDefinitionsNotificationEvent =
    void (*)(IvModuleDefinitionsNotification const &);

IV_DECLARE_LINKER_EVENT(
    IvPackageDeclarationsChangedEvent,
    iv_runtime_iv_package_declarations_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvNodeTypeDefinitionsChangedEvent,
    iv_runtime_iv_node_type_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleDefinitionsNotificationEvent,
    iv_runtime_iv_module_definitions_notification_event);
} // namespace iv
