#include <intravenous/runtime/configured_lanes_timeline_bridge.h>

#include <intravenous/runtime/configured_lanes.h>
#include <intravenous/runtime/configured_lanes_events.h>
#include <intravenous/runtime/timeline.h>

namespace iv {
IV_DEFINE_BRIDGE(configured_lanes_timeline_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    configured_lanes_timeline_bridge,
    iv_runtime_configured_lanes_timeline_batch_requested_event,
    &Timeline::handle_configured_lanes_timeline_batch)
IV_SUBSCRIBE_LINKER_EVENT(
    configured_lanes_timeline_bridge,
    iv_runtime_timeline_configured_lane_canonical_state_updated_event,
    &ConfiguredLanes::handle_timeline_configured_lane_canonical_state_updated)
IV_SUBSCRIBE_LINKER_EVENT(
    configured_lanes_timeline_bridge,
    iv_runtime_timeline_configured_lane_connection_recorded_event,
    &ConfiguredLanes::handle_timeline_configured_lane_connection_recorded)
IV_SUBSCRIBE_LINKER_EVENT(
    configured_lanes_timeline_bridge,
    iv_runtime_timeline_configured_lane_connection_removed_event,
    &ConfiguredLanes::handle_timeline_configured_lane_connection_removed)
IV_SUBSCRIBE_LINKER_EVENT(
    configured_lanes_timeline_bridge,
    iv_runtime_timeline_configured_lane_connections_requested_event,
    &ConfiguredLanes::handle_timeline_configured_lane_connections_requested)
} // namespace iv
