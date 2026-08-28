#include <intravenous/runtime/lane_query_schema_service.h>

#include <intravenous/runtime/lane_query_schema_events.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/timeline_events.h>

#include <intravenous/query/lane_query_completion.h>

#include <optional>
#include <utility>

namespace iv {
void LaneQuerySchemaService::initialize(query::LaneQuerySchema schema)
{
    std::scoped_lock lock(mutex);
    schema_ = std::move(schema);
}

void LaneQuerySchemaService::handle_timeline_lanes_changed(
    TimelineLanesChanged const &change)
{
    if (!change.schema_change.changed) {
        return;
    }

    std::optional<LaneQuerySchemaChanged> notification;
    {
        std::scoped_lock lock(mutex);
        // A newer snapshot has already been published, or this notification
        // was produced from a stale timeline revision.
        if (schema_.revision() != change.schema_change.old_revision) {
            return;
        }
        schema_ = change.schema;
        notification.emplace(LaneQuerySchemaChanged{
            .schema = change.schema,
            .change = change.schema_change,
        });
    }
    IV_INVOKE_LINKER_EVENT(iv_runtime_lane_query_schema_changed_event, *notification);
}

query::LaneQuerySchema LaneQuerySchemaService::snapshot() const
{
    std::scoped_lock lock(mutex);
    return schema_;
}

void LaneQuerySchemaService::handle_socket_rpc_get_lane_query_schema(
    GetLaneQuerySchemaRequest const &,
    SocketRpcLaneQuerySchemaResultBuilder &builder) const
{
    builder.succeed(snapshot());
}

void LaneQuerySchemaService::handle_socket_rpc_complete_lane_query(
    CompleteLaneQueryRequest const &request,
    SocketRpcLaneQueryCompletionResultBuilder &builder) const
{
    auto const schema = snapshot();
    // The client revision is advisory. A completion result is always computed
    // from one authoritative snapshot and returns that snapshot's revision,
    // allowing the client to discard stale work or refresh its cache.
    (void)request.schema_revision;
    builder.succeed(
        query::complete_lane_query(request.source, request.cursor_offset, schema),
        schema.revision());
}
} // namespace iv
