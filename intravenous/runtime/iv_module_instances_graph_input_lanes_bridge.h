#pragma once

namespace iv {
class GraphInputLanes;

void bind_iv_module_instances_graph_input_lanes_bridge(
    GraphInputLanes &lanes);
void unbind_iv_module_instances_graph_input_lanes_bridge(
    GraphInputLanes const &lanes);
} // namespace iv
