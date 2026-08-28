#pragma once

#include <intravenous/bridge.h>

namespace iv {
class GraphInputLanes;
class Timeline;

IV_DECLARE_BRIDGE(
    graph_input_lanes_timeline_bridge,
    GraphInputLanes,
    Timeline);
} // namespace iv
