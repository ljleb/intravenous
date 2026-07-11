#pragma once

namespace iv {
class LanesVisualization;
class Timeline;

void bind_lanes_visualization_timeline_bridge(LanesVisualization &visualization, Timeline &timeline);
void unbind_lanes_visualization_timeline_bridge(LanesVisualization const &visualization, Timeline const &timeline);
} // namespace iv
