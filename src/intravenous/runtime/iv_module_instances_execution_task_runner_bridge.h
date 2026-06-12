#pragma once

namespace iv {
class IvModuleInstancesExecution;
class TaskRunner;

void bind_iv_module_instances_execution_task_runner_bridge(
    IvModuleInstancesExecution &execution,
    TaskRunner &runner);
void unbind_iv_module_instances_execution_task_runner_bridge(
    IvModuleInstancesExecution const &execution,
    TaskRunner const &runner);
}
