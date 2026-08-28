#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleReload;
class TasksRunner;

IV_DECLARE_BRIDGE(task_runner_iv_module_reload_bridge, TasksRunner, IvModuleReload);
} // namespace iv
