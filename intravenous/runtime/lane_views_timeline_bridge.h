#pragma once

namespace iv {
class RuntimeLaneViews;

void bind_lane_views_timeline_bridge(RuntimeLaneViews &lane_views);
void unbind_lane_views_timeline_bridge(RuntimeLaneViews const &lane_views);
} // namespace iv
