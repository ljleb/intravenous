#pragma once

namespace iv {
class GraphInputLanes;

void bind_graph_input_lanes_lane_views_bridge(GraphInputLanes &graph_input_lanes);
void unbind_graph_input_lanes_lane_views_bridge(GraphInputLanes const &graph_input_lanes);
} // namespace iv
