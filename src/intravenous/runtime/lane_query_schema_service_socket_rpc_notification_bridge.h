#pragma once
#include <intravenous/bridge.h>
namespace iv {
class LaneQuerySchemaService;
class SocketRpcServer;
IV_DECLARE_BRIDGE(lane_query_schema_service_socket_rpc_notification_bridge, LaneQuerySchemaService, SocketRpcServer);
} // namespace iv
