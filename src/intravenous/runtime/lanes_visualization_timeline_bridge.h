#pragma once

#include <intravenous/bridge.h>

namespace iv {
class LanesVisualization;
class Timeline;

IV_DECLARE_BRIDGE(
    lanes_visualization_timeline_bridge,
    LanesVisualization,
    Timeline);
} // namespace iv
