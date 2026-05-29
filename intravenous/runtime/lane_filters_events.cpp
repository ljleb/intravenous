#include "runtime/lane_filters_events.h"

namespace iv {
IV_DEFINE_LINKER_EVENT(
    LaneFiltersChangedEvent,
    iv_runtime_lane_filters_changed_event);
IV_DEFINE_LINKER_EVENT(
    LaneFilterStoredEvent,
    iv_runtime_lane_filter_stored_event);
IV_DEFINE_LINKER_EVENT(
    LaneFilterRemovedEvent,
    iv_runtime_lane_filter_removed_event);
} // namespace iv
