#pragma once

#include "linker_event.h"
#include "runtime/iv_modules.h"

namespace iv {
using RuntimeIvModulesDefinitionsChangedEvent =
    void (*)(RuntimeIvModuleDefinitionsChanged const &);
using RuntimeIvModulesNotificationEvent =
    void (*)(RuntimeIvModulesNotification const &);

IV_DECLARE_LINKER_EVENT(
    RuntimeIvModulesDefinitionsChangedEvent,
    iv_runtime_iv_modules_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeIvModulesNotificationEvent,
    iv_runtime_iv_modules_notification_event);
} // namespace iv
