#pragma once

namespace iv {
class GraphInputLanes;

void bind_task_runner_graph_input_lanes_bridge(
    GraphInputLanes &lanes);
void unbind_task_runner_graph_input_lanes_bridge(
    GraphInputLanes const &lanes);
} // namespace iv
