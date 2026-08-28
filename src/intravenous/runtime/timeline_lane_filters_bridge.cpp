#include <intravenous/runtime/timeline_lane_filters_bridge.h>

#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/timeline_events.h>

namespace iv {
IV_DEFINE_BRIDGE(timeline_lane_filters_bridge)
IV_SUBSCRIBE_BRIDGE(
    timeline_lane_filters_bridge,
    iv_runtime_timeline_lanes_changed_event,
    &LaneFilters::handle_timeline_lanes_changed);
} // namespace iv
