#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ProjectPersistence;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_project_persistence_bridge,
    SocketRpcServer,
    ProjectPersistence);
} // namespace iv
