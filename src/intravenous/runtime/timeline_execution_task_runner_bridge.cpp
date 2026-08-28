#include <intravenous/runtime/timeline_execution_task_runner_bridge.h>

#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/timeline_execution.h>
#include <intravenous/runtime/timeline_execution_events.h>

namespace iv {
IV_DEFINE_BRIDGE(timeline_execution_task_runner_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    timeline_execution_task_runner_bridge,
    iv_runtime_timeline_execution_tasks_changed_event,
    (static_cast<void (TasksRunner::*)(VersionedTaskGraphUpdate const &)>(
        &TasksRunner::update_tasks)));
} // namespace iv
