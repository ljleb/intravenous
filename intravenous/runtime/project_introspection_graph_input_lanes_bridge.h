#pragma once

namespace iv {
class RuntimeGraphInputLanes;

void bind_project_introspection_graph_input_lanes_bridge(RuntimeGraphInputLanes &lanes);
void unbind_project_introspection_graph_input_lanes_bridge(RuntimeGraphInputLanes const &lanes);
} // namespace iv
