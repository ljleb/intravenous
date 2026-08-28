#pragma once

#include <intravenous/bridge.h>

namespace iv {
class TimelineExecution;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_timeline_execution_bridge,
    SocketRpcServer,
    TimelineExecution);
} // namespace iv
