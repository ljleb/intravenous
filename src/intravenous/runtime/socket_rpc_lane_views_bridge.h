#pragma once

#include <intravenous/bridge.h>

namespace iv {
class LaneViews;
class SocketRpcServer;

IV_DECLARE_BRIDGE(socket_rpc_lane_views_bridge, SocketRpcServer, LaneViews);
} // namespace iv
