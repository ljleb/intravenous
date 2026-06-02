#pragma once

namespace iv {
class LaneFilters;

void bind_timeline_lane_filters_bridge(LaneFilters &lane_filters);
void unbind_timeline_lane_filters_bridge(LaneFilters const &lane_filters);
} // namespace iv
