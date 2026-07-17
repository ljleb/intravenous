#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/query/lane_query_schema.h>

namespace iv {
struct LaneQuerySchemaChanged {
    query::LaneQuerySchema schema {};
    query::LaneQuerySchemaChange change {};
};

using LaneQuerySchemaChangedEvent = void (*)(LaneQuerySchemaChanged const &);

IV_DECLARE_LINKER_EVENT(
    LaneQuerySchemaChangedEvent,
    iv_runtime_lane_query_schema_changed_event);
} // namespace iv
