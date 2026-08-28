#include <intravenous/runtime/timeline_lane_query_schema_bridge.h>

#include <intravenous/runtime/lane_query_schema_service.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_events.h>

namespace iv {
IV_DEFINE_BRIDGE(timeline_lane_query_schema_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    timeline_lane_query_schema_bridge,
    iv_runtime_timeline_lanes_changed_event,
    &LaneQuerySchemaService::handle_timeline_lanes_changed);
} // namespace iv
