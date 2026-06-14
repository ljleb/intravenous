#pragma once

namespace iv {
class LanesVisualization;

void bind_lane_views_lanes_visualization_bridge(LanesVisualization &visualization);
void unbind_lane_views_lanes_visualization_bridge(LanesVisualization const &visualization);
} // namespace iv
