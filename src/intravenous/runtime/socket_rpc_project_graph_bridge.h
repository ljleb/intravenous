#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ProjectGraph;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_project_graph_bridge,
    SocketRpcServer,
    ProjectGraph);
} // namespace iv
