#include <intravenous/runtime/iv_module_instances_execution_task_runner_bridge.h>

#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_execution_events.h>
#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/task_runner_events.h>

namespace iv {
namespace {
IvModuleInstancesExecution *bound_execution = nullptr;
TasksRunner *bound_runner = nullptr;

void handle_iv_module_instances_execution_tasks_changed(VersionedTaskGraphUpdate const &update)
{
    if (!bound_execution || !bound_runner) {
        return;
    }
    bound_runner->update_tasks(update);
}

void handle_task_runner_after_pass(TasksRunnerAfterPass const& pass)
{
    if (!bound_execution || !bound_runner) return;
    bound_execution->observe_completed_graph_revision(pass.graph_revision);
    (void)bound_runner->activate_deferred_graph_after([&] {
        bound_execution->commit_prepared_reloads(pass.graph_revision);
    });
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleInstancesExecutionTasksChangedEvent,
    iv_runtime_iv_module_instances_execution_tasks_changed_event,
    handle_iv_module_instances_execution_tasks_changed);
IV_SUBSCRIBE_LINKER_EVENT(
    TasksRunnerAfterPassEvent,
    iv_runtime_task_runner_after_pass_event,
    handle_task_runner_after_pass);
}

void bind_iv_module_instances_execution_task_runner_bridge(
    IvModuleInstancesExecution &execution,
    TasksRunner &runner)
{
    bound_execution = &execution;
    bound_runner = &runner;
}

void unbind_iv_module_instances_execution_task_runner_bridge(
    IvModuleInstancesExecution const &execution,
    TasksRunner const &runner)
{
    if (bound_execution == &execution) {
        bound_execution = nullptr;
    }
    if (bound_runner == &runner) {
        bound_runner = nullptr;
    }
}
}
