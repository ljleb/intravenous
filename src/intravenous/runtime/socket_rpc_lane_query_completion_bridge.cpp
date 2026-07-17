#include <intravenous/runtime/socket_rpc_lane_query_completion_bridge.h>

#include <intravenous/query/lane_query_completion.h>
#include <intravenous/runtime/lane_query_schema_service.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
namespace {
LaneQuerySchemaService *bound_service = nullptr;

void handle_complete_lane_query(
    CompleteLaneQueryRequest const &request,
    SocketRpcLaneQueryCompletionResultBuilder &builder)
{
    if (bound_service == nullptr) {
        builder.fail("lane query schema service is unavailable");
        return;
    }
    auto const schema = bound_service->snapshot();
    // The client revision is advisory. A completion result is always computed
    // from one authoritative snapshot and returns that snapshot's revision,
    // allowing the client to discard stale work or refresh its cache.
    (void)request.schema_revision;
    builder.succeed(
        query::complete_lane_query(request.source, request.cursor_offset, schema),
        schema.revision());
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcCompleteLaneQueryEvent,
    iv_socket_rpc_complete_lane_query_event,
    handle_complete_lane_query);
} // namespace

void bind_socket_rpc_lane_query_completion_bridge(LaneQuerySchemaService &service)
{
    bound_service = &service;
}

void unbind_socket_rpc_lane_query_completion_bridge(LaneQuerySchemaService const &service)
{
    if (bound_service == &service) {
        bound_service = nullptr;
    }
}
} // namespace iv
