#pragma once

namespace iv {
class LaneViews;

void bind_runtime_project_lane_views_bridge(LaneViews &lane_views);
void unbind_runtime_project_lane_views_bridge(LaneViews const &lane_views);
} // namespace iv
