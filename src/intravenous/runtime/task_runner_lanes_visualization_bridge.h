#pragma once

#include <intravenous/bridge.h>

namespace iv {
class LanesVisualization;
class TasksRunner;

IV_DECLARE_BRIDGE(
    task_runner_lanes_visualization_bridge,
    TasksRunner,
    LanesVisualization);
} // namespace iv
