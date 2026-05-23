#pragma once

namespace iv {
class RuntimeGraphInputLanes;

void bind_iv_module_instances_graph_input_lanes_bridge(
    RuntimeGraphInputLanes &lanes);
void unbind_iv_module_instances_graph_input_lanes_bridge(
    RuntimeGraphInputLanes const &lanes);
} // namespace iv
