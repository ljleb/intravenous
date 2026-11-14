#pragma once

#include <intravenous/bridge.h>

namespace iv {
class NodeInstances;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_node_instances_bridge,
    SocketRpcServer,
    NodeInstances);
} // namespace iv
