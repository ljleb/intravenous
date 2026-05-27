#pragma once

namespace iv {
class GraphInputLanes;

void bind_project_introspection_graph_input_lanes_bridge(GraphInputLanes &lanes);
void unbind_project_introspection_graph_input_lanes_bridge(GraphInputLanes const &lanes);
} // namespace iv
