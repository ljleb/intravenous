#include <intravenous/runtime/socket_rpc_lane_query_schema_bridge.h>

#include <intravenous/runtime/lane_query_schema_service.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
namespace {
LaneQuerySchemaService *bound_service = nullptr;

void handle_get_lane_query_schema(
    GetLaneQuerySchemaRequest const &,
    SocketRpcLaneQuerySchemaResultBuilder &builder)
{
    if (bound_service == nullptr) {
        builder.fail("lane query schema service is unavailable");
        return;
    }
    builder.succeed(bound_service->snapshot());
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGetLaneQuerySchemaEvent,
    iv_socket_rpc_get_lane_query_schema_event,
    handle_get_lane_query_schema);
} // namespace

void bind_socket_rpc_lane_query_schema_bridge(LaneQuerySchemaService &service)
{
    bound_service = &service;
}

void unbind_socket_rpc_lane_query_schema_bridge(LaneQuerySchemaService const &service)
{
    if (bound_service == &service) {
        bound_service = nullptr;
    }
}
} // namespace iv
