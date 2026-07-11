#include <intravenous/runtime/lane_filters_lane_views_bridge.h>

#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/lane_filters_events.h>
#include <intravenous/runtime/lane_views.h>

namespace iv {
namespace {
LaneFilters *bound_lane_filters = nullptr;
LaneViews *bound_lane_views = nullptr;

void handle_lane_filters_changed(LaneFiltersChanged const &change)
{
    if (bound_lane_views == nullptr) {
        return;
    }
    bound_lane_views->handle_lane_filters_changed(change);
}

void handle_lane_filter_stored(LaneFilterStoredRequest const &request)
{
    if (bound_lane_filters == nullptr) {
        return;
    }
    bound_lane_filters->store_filter(request);
}

void handle_lane_filter_removed(std::string const &filter_name)
{
    if (bound_lane_filters == nullptr) {
        return;
    }
    bound_lane_filters->remove_filter(filter_name);
}

IV_SUBSCRIBE_LINKER_EVENT(
    LaneFiltersChangedEvent,
    iv_runtime_lane_filters_changed_event,
    handle_lane_filters_changed);
IV_SUBSCRIBE_LINKER_EVENT(
    LaneFilterStoredEvent,
    iv_runtime_lane_filter_stored_event,
    handle_lane_filter_stored);
IV_SUBSCRIBE_LINKER_EVENT(
    LaneFilterRemovedEvent,
    iv_runtime_lane_filter_removed_event,
    handle_lane_filter_removed);
} // namespace

void bind_lane_filters_lane_views_bridge(LaneFilters &lane_filters, LaneViews &lane_views)
{
    bound_lane_filters = &lane_filters;
    bound_lane_views = &lane_views;
}

void unbind_lane_filters_lane_views_bridge(LaneFilters const &lane_filters, LaneViews const &lane_views)
{
    if (bound_lane_filters == &lane_filters) {
        bound_lane_filters = nullptr;
    }
    if (bound_lane_views == &lane_views) {
        bound_lane_views = nullptr;
    }
}
} // namespace iv
