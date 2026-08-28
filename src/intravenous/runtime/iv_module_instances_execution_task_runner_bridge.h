#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleInstancesExecution;
class TasksRunner;

IV_DECLARE_BRIDGE(
    iv_module_instances_execution_task_runner_bridge,
    IvModuleInstancesExecution,
    TasksRunner);
}
