#include <intravenous/runtime/configured_lanes_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    ConfiguredLanesTimelineBatchRequestedEvent,
    iv_runtime_configured_lanes_timeline_batch_requested_event);
IV_DEFINE_LINKER_EVENT(
    TimelineConfiguredLaneCanonicalStateUpdatedEvent,
    iv_runtime_timeline_configured_lane_canonical_state_updated_event);
IV_DEFINE_LINKER_EVENT(
    TimelineConfiguredLaneConnectionRecordedEvent,
    iv_runtime_timeline_configured_lane_connection_recorded_event);
IV_DEFINE_LINKER_EVENT(
    TimelineConfiguredLaneConnectionRemovedEvent,
    iv_runtime_timeline_configured_lane_connection_removed_event);
IV_DEFINE_LINKER_EVENT(
    TimelineConfiguredLaneConnectionsRequestedEvent,
    iv_runtime_timeline_configured_lane_connections_requested_event);
} // namespace iv
