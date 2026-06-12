#include <intravenous/runtime/timeline_execution_task_runner_bridge.h>

#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/timeline_execution.h>
#include <intravenous/runtime/timeline_execution_events.h>

namespace iv {
namespace {
TimelineExecution *bound_execution = nullptr;
TaskRunner *bound_runner = nullptr;

void handle_timeline_execution_tasks_changed(TaskGraphUpdate const &update)
{
    if (!bound_execution || !bound_runner) {
        return;
    }
    bound_runner->update_tasks(update);
}

IV_SUBSCRIBE_LINKER_EVENT(
    TimelineExecutionTasksChangedEvent,
    iv_runtime_timeline_execution_tasks_changed_event,
    handle_timeline_execution_tasks_changed);
}

void bind_timeline_execution_task_runner_bridge(TimelineExecution &execution, TaskRunner &runner)
{
    bound_execution = &execution;
    bound_runner = &runner;
}

void unbind_timeline_execution_task_runner_bridge(TimelineExecution const &execution, TaskRunner const &runner)
{
    if (bound_execution == &execution) {
        bound_execution = nullptr;
    }
    if (bound_runner == &runner) {
        bound_runner = nullptr;
    }
}
}
