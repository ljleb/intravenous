#pragma once

#include <intravenous/bridge.h>

namespace iv {
class LaneViews;
class LanesVisualization;

IV_DECLARE_BRIDGE(lane_views_lanes_visualization_bridge, LaneViews, LanesVisualization);
} // namespace iv
