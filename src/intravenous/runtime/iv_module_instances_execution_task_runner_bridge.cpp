#include <intravenous/runtime/iv_module_instances_execution_task_runner_bridge.h>

#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_execution_events.h>
#include <intravenous/runtime/task_runner.h>

namespace iv {
namespace {
IvModuleInstancesExecution *bound_execution = nullptr;
TaskRunner *bound_runner = nullptr;

void handle_iv_module_instances_execution_tasks_changed(TaskGraphUpdate const &update)
{
    if (!bound_execution || !bound_runner) {
        return;
    }
    bound_runner->update_tasks(update);
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleInstancesExecutionTasksChangedEvent,
    iv_runtime_iv_module_instances_execution_tasks_changed_event,
    handle_iv_module_instances_execution_tasks_changed);
}

void bind_iv_module_instances_execution_task_runner_bridge(
    IvModuleInstancesExecution &execution,
    TaskRunner &runner)
{
    bound_execution = &execution;
    bound_runner = &runner;
}

void unbind_iv_module_instances_execution_task_runner_bridge(
    IvModuleInstancesExecution const &execution,
    TaskRunner const &runner)
{
    if (bound_execution == &execution) {
        bound_execution = nullptr;
    }
    if (bound_runner == &runner) {
        bound_runner = nullptr;
    }
}
}
