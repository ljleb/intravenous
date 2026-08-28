#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ProjectPersistence;
class SocketRpcServer;
IV_DECLARE_BRIDGE(project_persistence_socket_rpc_notification_bridge, ProjectPersistence, SocketRpcServer);
} // namespace iv
