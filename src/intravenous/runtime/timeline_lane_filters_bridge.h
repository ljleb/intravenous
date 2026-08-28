#pragma once

#include <intravenous/bridge.h>

namespace iv {
class LaneFilters;
class Timeline;

IV_DECLARE_BRIDGE(timeline_lane_filters_bridge, Timeline, LaneFilters);
} // namespace iv
