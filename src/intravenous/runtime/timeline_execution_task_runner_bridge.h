#pragma once

#include <intravenous/bridge.h>

namespace iv {
class TimelineExecution;
class TasksRunner;

IV_DECLARE_BRIDGE(
    timeline_execution_task_runner_bridge,
    TimelineExecution,
    TasksRunner);
} // namespace iv
