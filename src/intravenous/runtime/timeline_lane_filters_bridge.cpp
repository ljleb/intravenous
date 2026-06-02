#include "runtime/timeline_lane_filters_bridge.h"

#include "runtime/lane_filters.h"
#include "runtime/timeline_events.h"

namespace iv {
namespace {
LaneFilters *bound_lane_filters = nullptr;

void handle_timeline_lanes_changed(TimelineLanesChanged const &change)
{
    if (bound_lane_filters == nullptr) {
        return;
    }
    bound_lane_filters->handle_timeline_lanes_changed(change);
}

IV_SUBSCRIBE_LINKER_EVENT(
    TimelineLanesChangedEvent,
    iv_runtime_timeline_lanes_changed_event,
    handle_timeline_lanes_changed);
} // namespace

void bind_timeline_lane_filters_bridge(LaneFilters &lane_filters)
{
    bound_lane_filters = &lane_filters;
}

void unbind_timeline_lane_filters_bridge(LaneFilters const &lane_filters)
{
    if (bound_lane_filters == &lane_filters) {
        bound_lane_filters = nullptr;
    }
}
} // namespace iv
