#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ProjectAutosave;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_project_autosave_bridge,
    SocketRpcServer,
    ProjectAutosave);
} // namespace iv
