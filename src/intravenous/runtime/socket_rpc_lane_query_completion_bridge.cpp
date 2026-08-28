#include <intravenous/runtime/socket_rpc_lane_query_completion_bridge.h>

#include <intravenous/query/lane_query_completion.h>
#include <intravenous/runtime/lane_query_schema_service.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_lane_query_completion_bridge)

IV_SUBSCRIBE_BRIDGE(
    socket_rpc_lane_query_completion_bridge,
    iv_socket_rpc_complete_lane_query_event,
    &LaneQuerySchemaService::handle_socket_rpc_complete_lane_query);
} // namespace iv
