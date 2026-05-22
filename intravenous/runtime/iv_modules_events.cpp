#include "runtime/iv_modules_events.h"

namespace iv {
IV_DEFINE_LINKER_EVENT(
    RuntimeIvModulesDefinitionsChangedEvent,
    iv_runtime_iv_modules_definitions_changed_event);
IV_DEFINE_LINKER_EVENT(
    RuntimeIvModulesNotificationEvent,
    iv_runtime_iv_modules_notification_event);
} // namespace iv
