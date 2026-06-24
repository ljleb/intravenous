#pragma once

namespace iv {
class GraphInputLanes;

void bind_runtime_project_graph_input_lanes_bridge(GraphInputLanes &graph_input_lanes);
void unbind_runtime_project_graph_input_lanes_bridge(GraphInputLanes const &graph_input_lanes);
} // namespace iv
