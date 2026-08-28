#pragma once

#include <intravenous/bridge.h>

namespace iv {
class GraphInputLanes;
class TasksRunner;

IV_DECLARE_BRIDGE(
    task_runner_graph_input_lanes_bridge,
    TasksRunner,
    GraphInputLanes);
} // namespace iv
