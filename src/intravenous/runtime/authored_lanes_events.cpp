#include <intravenous/runtime/authored_lanes_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    AuthoredLanesTimelineBatchRequestedEvent,
    iv_runtime_authored_lanes_timeline_batch_requested_event);
IV_DEFINE_LINKER_EVENT(
    TimelineAuthoredLaneCanonicalStateUpdatedEvent,
    iv_runtime_timeline_authored_lane_canonical_state_updated_event);
IV_DEFINE_LINKER_EVENT(
    TimelineAuthoredLaneConnectionRecordedEvent,
    iv_runtime_timeline_authored_lane_connection_recorded_event);
IV_DEFINE_LINKER_EVENT(
    TimelineAuthoredLaneConnectionRemovedEvent,
    iv_runtime_timeline_authored_lane_connection_removed_event);
IV_DEFINE_LINKER_EVENT(
    TimelineAuthoredLaneConnectionsRequestedEvent,
    iv_runtime_timeline_authored_lane_connections_requested_event);
} // namespace iv
