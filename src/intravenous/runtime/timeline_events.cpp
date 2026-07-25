#include <intravenous/runtime/timeline_events.h>

namespace iv {
    IV_DEFINE_LINKER_EVENT(
        TimelineLanesChangedEvent,
        iv_runtime_timeline_lanes_changed_event);
    IV_DEFINE_LINKER_EVENT(
        TimelineLaneBatchRequestedEvent,
        iv_runtime_timeline_lane_batch_requested_event);
} // namespace iv
