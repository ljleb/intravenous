#pragma once

namespace iv {
class LaneViews;

void bind_socket_rpc_lane_views_bridge(LaneViews &lane_views);
void unbind_socket_rpc_lane_views_bridge(LaneViews const &lane_views);
} // namespace iv
