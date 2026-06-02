#pragma once

namespace iv {
class LaneFilters;
class LaneViews;

void bind_lane_filters_lane_views_bridge(LaneFilters &lane_filters, LaneViews &lane_views);
void unbind_lane_filters_lane_views_bridge(LaneFilters const &lane_filters, LaneViews const &lane_views);
} // namespace iv
