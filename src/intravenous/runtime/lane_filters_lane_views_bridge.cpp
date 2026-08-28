#include <intravenous/runtime/lane_filters_lane_views_bridge.h>

#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/lane_filters_events.h>
#include <intravenous/runtime/lane_views.h>

namespace iv {
IV_DEFINE_BRIDGE(lane_filters_lane_views_bridge)
// lane_filters <-> lane_views bridge.
//
//   LaneFiltersChanged -> LaneViews::handle_lane_filters_changed
//   LaneFilterStored   -> LaneFilters::store_filter
//   LaneFilterRemoved  -> LaneFilters::remove_filter
IV_SUBSCRIBE_LINKER_EVENT(
    lane_filters_lane_views_bridge,
    iv_runtime_lane_filters_changed_event,
    &LaneViews::handle_lane_filters_changed);
IV_SUBSCRIBE_LINKER_EVENT(
    lane_filters_lane_views_bridge,
    iv_runtime_lane_filter_stored_event,
    &LaneFilters::store_filter);
IV_SUBSCRIBE_LINKER_EVENT(
    lane_filters_lane_views_bridge,
    iv_runtime_lane_filter_removed_event,
    &LaneFilters::remove_filter);
} // namespace iv
