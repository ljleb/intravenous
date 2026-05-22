#pragma once

namespace iv {
class Timeline;

void bind_graph_input_lanes_timeline_bridge(Timeline &timeline);
void unbind_graph_input_lanes_timeline_bridge(Timeline const &timeline);
} // namespace iv
