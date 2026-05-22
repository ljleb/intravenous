#pragma once

namespace iv {
class RuntimeGraphInputLanes;

void bind_graph_input_lanes_lane_views_bridge(RuntimeGraphInputLanes &graph_input_lanes);
void unbind_graph_input_lanes_lane_views_bridge(RuntimeGraphInputLanes const &graph_input_lanes);
} // namespace iv
