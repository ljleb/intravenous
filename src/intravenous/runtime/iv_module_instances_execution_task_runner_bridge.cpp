#include <intravenous/runtime/iv_module_instances_execution_task_runner_bridge.h>

#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_execution_events.h>
#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/task_runner_events.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_instances_execution_task_runner_bridge)

IV_SUBSCRIBE_BRIDGE(
    iv_module_instances_execution_task_runner_bridge,
    iv_runtime_iv_module_instances_execution_tasks_changed_event,
    static_cast<void (TasksRunner::*)(VersionedTaskGraphUpdate const &)>(
        &TasksRunner::update_tasks));
IV_SUBSCRIBE_BRIDGE(
    iv_module_instances_execution_task_runner_bridge,
    iv_runtime_task_runner_after_pass_event,
    &IvModuleInstancesExecution::handle_task_runner_after_pass);
}
