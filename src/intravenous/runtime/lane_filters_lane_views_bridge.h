#pragma once

#include <intravenous/bridge.h>

namespace iv {
class LaneFilters;
class LaneViews;

IV_DECLARE_BRIDGE(
    lane_filters_lane_views_bridge,
    LaneFilters,
    LaneViews);
} // namespace iv
