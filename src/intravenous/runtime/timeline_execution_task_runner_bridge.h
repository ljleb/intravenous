#pragma once

namespace iv {
class TimelineExecution;
class TasksRunner;

void bind_timeline_execution_task_runner_bridge(TimelineExecution &execution, TasksRunner &runner);
void unbind_timeline_execution_task_runner_bridge(TimelineExecution const &execution, TasksRunner const &runner);
}
