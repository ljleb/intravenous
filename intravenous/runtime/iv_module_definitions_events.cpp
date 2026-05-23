#include "runtime/iv_module_definitions_events.h"

namespace iv {
IV_DEFINE_LINKER_EVENT(
    RuntimeIvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event);
IV_DEFINE_LINKER_EVENT(
    RuntimeIvModuleDefinitionsNotificationEvent,
    iv_runtime_iv_module_definitions_notification_event);
} // namespace iv
