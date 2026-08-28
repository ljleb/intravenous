#pragma once
#include <intravenous/bridge.h>
namespace iv {
class LaneViews;
class SocketRpcServer;
IV_DECLARE_BRIDGE(lane_views_socket_rpc_notification_bridge, LaneViews, SocketRpcServer);
} // namespace iv
