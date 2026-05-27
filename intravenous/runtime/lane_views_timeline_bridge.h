#pragma once

namespace iv {
class LaneViews;

void bind_lane_views_timeline_bridge(LaneViews &lane_views);
void unbind_lane_views_timeline_bridge(LaneViews const &lane_views);
} // namespace iv
