#include "runtime/lane_views_timeline_bridge.h"

#include "runtime/lane_views.h"
#include "runtime/timeline_events.h"

namespace iv {
namespace {
    LaneViews *bound_lane_views = nullptr;

    void handle_timeline_lanes_changed(TimelineLanesChanged const &change)
    {
        if (bound_lane_views == nullptr) {
            return;
        }
        bound_lane_views->handle_timeline_lanes_changed(change);
    }

    IV_SUBSCRIBE_LINKER_EVENT(
        TimelineLanesChangedEvent,
        iv_runtime_timeline_lanes_changed_event,
        handle_timeline_lanes_changed);
}

void bind_lane_views_timeline_bridge(LaneViews &lane_views)
{
    bound_lane_views = &lane_views;
}

void unbind_lane_views_timeline_bridge(LaneViews const &lane_views)
{
    if (bound_lane_views == &lane_views) {
        bound_lane_views = nullptr;
    }
}
} // namespace iv
