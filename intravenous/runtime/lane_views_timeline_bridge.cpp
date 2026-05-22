#include "runtime/lane_views_timeline_bridge.h"

#include "runtime/lane_views.h"
#include "runtime/timeline_events.h"

namespace iv {
namespace {
    RuntimeLaneViews *bound_lane_views = nullptr;

    void handle_timeline_lanes_changed(RuntimeTimelineLanesChanged const &change)
    {
        if (bound_lane_views == nullptr) {
            return;
        }
        bound_lane_views->handle_timeline_lanes_changed(change);
    }

    IV_SUBSCRIBE_LINKER_EVENT(
        RuntimeTimelineLanesChangedEvent,
        iv_runtime_timeline_lanes_changed_event,
        handle_timeline_lanes_changed);
}

void bind_lane_views_timeline_bridge(RuntimeLaneViews &lane_views)
{
    bound_lane_views = &lane_views;
}

void unbind_lane_views_timeline_bridge(RuntimeLaneViews const &lane_views)
{
    if (bound_lane_views == &lane_views) {
        bound_lane_views = nullptr;
    }
}
} // namespace iv
