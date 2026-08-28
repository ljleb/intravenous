#include <intravenous/runtime/authored_lanes_timeline_bridge.h>

#include <intravenous/runtime/authored_lanes.h>
#include <intravenous/runtime/authored_lanes_events.h>
#include <intravenous/runtime/timeline.h>

namespace iv {
IV_DEFINE_BRIDGE(authored_lanes_timeline_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    authored_lanes_timeline_bridge,
    iv_runtime_authored_lanes_timeline_batch_requested_event,
    &Timeline::handle_authored_lanes_timeline_batch)
IV_SUBSCRIBE_LINKER_EVENT(
    authored_lanes_timeline_bridge,
    iv_runtime_timeline_authored_lane_canonical_state_updated_event,
    &AuthoredLanes::handle_timeline_authored_lane_canonical_state_updated)
IV_SUBSCRIBE_LINKER_EVENT(
    authored_lanes_timeline_bridge,
    iv_runtime_timeline_authored_lane_connection_recorded_event,
    &AuthoredLanes::handle_timeline_authored_lane_connection_recorded)
IV_SUBSCRIBE_LINKER_EVENT(
    authored_lanes_timeline_bridge,
    iv_runtime_timeline_authored_lane_connection_removed_event,
    &AuthoredLanes::handle_timeline_authored_lane_connection_removed)
IV_SUBSCRIBE_LINKER_EVENT(
    authored_lanes_timeline_bridge,
    iv_runtime_timeline_authored_lane_connections_requested_event,
    &AuthoredLanes::handle_timeline_authored_lane_connections_requested)
} // namespace iv
