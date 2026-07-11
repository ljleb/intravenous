#include <intravenous/runtime/task_runner_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    TasksRunnerBeforePassEvent,
    iv_runtime_task_runner_before_pass_event);
IV_DEFINE_LINKER_EVENT(
    TasksRunnerAfterPassEvent,
    iv_runtime_task_runner_after_pass_event);
} // namespace iv
