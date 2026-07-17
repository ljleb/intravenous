#include <intravenous/runtime/lane_query_schema_service.h>

#include <intravenous/runtime/lane_query_schema_events.h>
#include <intravenous/runtime/timeline.h>

#include <optional>
#include <utility>

namespace iv {
void LaneQuerySchemaService::initialize(query::LaneQuerySchema schema)
{
    std::scoped_lock lock(mutex);
    schema_ = std::move(schema);
}

void LaneQuerySchemaService::handle_timeline_lanes_changed(Timeline &timeline)
{
    query::LaneQuerySchema previous;
    {
        std::scoped_lock lock(mutex);
        previous = schema_;
    }
    auto candidate = timeline.lane_query_schema(previous.revision() + 1);
    auto change = query::diff_lane_query_schemas(previous, candidate);
    if (!change.changed) {
        return;
    }

    std::optional<LaneQuerySchemaChanged> notification;
    {
        std::scoped_lock lock(mutex);
        // A concurrent lane change has already published a newer complete
        // snapshot. Its notification is sufficient; do not publish an
        // out-of-order revision from this older observation.
        if (schema_.revision() != previous.revision()) {
            return;
        }
        schema_ = candidate;
        notification.emplace(LaneQuerySchemaChanged{
            .schema = std::move(candidate),
            .change = std::move(change),
        });
    }
    IV_INVOKE_LINKER_EVENT(iv_runtime_lane_query_schema_changed_event, *notification);
}

query::LaneQuerySchema LaneQuerySchemaService::snapshot() const
{
    std::scoped_lock lock(mutex);
    return schema_;
}
} // namespace iv
