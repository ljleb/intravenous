#pragma once

namespace iv {
class RuntimeLaneViews;

void bind_socket_rpc_lane_views_bridge(RuntimeLaneViews &lane_views);
void unbind_socket_rpc_lane_views_bridge(RuntimeLaneViews const &lane_views);
} // namespace iv
