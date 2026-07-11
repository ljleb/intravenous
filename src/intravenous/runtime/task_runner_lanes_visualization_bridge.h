#pragma once

namespace iv {
class LanesVisualization;

void bind_task_runner_lanes_visualization_bridge(LanesVisualization &visualization);
void unbind_task_runner_lanes_visualization_bridge(LanesVisualization const &visualization);
} // namespace iv
