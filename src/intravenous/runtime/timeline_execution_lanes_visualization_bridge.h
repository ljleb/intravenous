#pragma once

#include <intravenous/bridge.h>

namespace iv {
class LanesVisualization;
class TimelineExecution;

IV_DECLARE_BRIDGE(
    timeline_execution_lanes_visualization_bridge,
    TimelineExecution,
    LanesVisualization);
} // namespace iv
