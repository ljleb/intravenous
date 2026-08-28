#pragma once
#include <intravenous/bridge.h>
namespace iv {
class LanesVisualization;
class SocketRpcServer;
IV_DECLARE_BRIDGE(lanes_visualization_socket_rpc_notification_bridge, LanesVisualization, SocketRpcServer);
} // namespace iv
