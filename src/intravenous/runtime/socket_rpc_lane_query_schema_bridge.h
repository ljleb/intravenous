#pragma once

#include <intravenous/bridge.h>

namespace iv {
class LaneQuerySchemaService;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_lane_query_schema_bridge,
    SocketRpcServer,
    LaneQuerySchemaService);
} // namespace iv
