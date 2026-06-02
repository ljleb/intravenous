#pragma once

namespace iv {
class GraphInputLanes;
class Timeline;

void bind_graph_input_lanes_timeline_bridge(GraphInputLanes &graph_input_lanes, Timeline &timeline);
void unbind_graph_input_lanes_timeline_bridge(
    GraphInputLanes const &graph_input_lanes,
    Timeline const &timeline);
} // namespace iv
