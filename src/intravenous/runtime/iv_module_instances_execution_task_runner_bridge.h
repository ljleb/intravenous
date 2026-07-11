#pragma once

namespace iv {
class IvModuleInstancesExecution;
class TasksRunner;

void bind_iv_module_instances_execution_task_runner_bridge(
    IvModuleInstancesExecution &execution,
    TasksRunner &runner);
void unbind_iv_module_instances_execution_task_runner_bridge(
    IvModuleInstancesExecution const &execution,
    TasksRunner const &runner);
}
