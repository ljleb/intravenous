#pragma once

namespace iv {
class TimelineExecution;
class TaskRunner;

void bind_timeline_execution_task_runner_bridge(TimelineExecution &execution, TaskRunner &runner);
void unbind_timeline_execution_task_runner_bridge(TimelineExecution const &execution, TaskRunner const &runner);
}
