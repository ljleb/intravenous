#include <intravenous/runtime/socket_rpc_lane_query_schema_bridge.h>

#include <intravenous/runtime/lane_query_schema_service.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <intravenous/runtime/lane_query_schema_events.h>
namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_lane_query_schema_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_lane_query_schema_bridge,
    iv_socket_rpc_get_lane_query_schema_event,
    &LaneQuerySchemaService::handle_socket_rpc_get_lane_query_schema);
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_lane_query_schema_bridge,
    iv_runtime_lane_query_schema_changed_event,
    &SocketRpcServer::handle_lane_query_schema_changed)
} // namespace iv
