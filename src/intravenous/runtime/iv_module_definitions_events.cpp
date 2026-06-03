#include <intravenous/runtime/iv_module_definitions_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    IvModuleDefinitionDeclarationsChangedEvent,
    iv_runtime_iv_module_definitions_declarations_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleDefinitionsNotificationEvent,
    iv_runtime_iv_module_definitions_notification_event);
} // namespace iv
