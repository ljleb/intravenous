#include <intravenous/runtime/lane_views_lanes_visualization_bridge.h>

#include <intravenous/runtime/lane_views_events.h>
#include <intravenous/runtime/lanes_visualization.h>

namespace iv {
namespace {
LanesVisualization *bound_visualization = nullptr;

void handle_lane_views_updated(LaneViewResult const &update)
{
    if (bound_visualization == nullptr) {
        return;
    }
    bound_visualization->handle_lane_views_updated(update);
}

void handle_lane_view_closed(std::string const &view_id)
{
    if (bound_visualization == nullptr) {
        return;
    }
    bound_visualization->handle_lane_view_closed(
        InternedString::from_string(view_id));
}

IV_SUBSCRIBE_LINKER_EVENT(
    LaneViewsUpdatedEvent,
    iv_runtime_lane_views_updated_event,
    handle_lane_views_updated);
IV_SUBSCRIBE_LINKER_EVENT(
    LaneViewClosedEvent,
    iv_runtime_lane_view_closed_event,
    handle_lane_view_closed);
} // namespace

void bind_lane_views_lanes_visualization_bridge(LanesVisualization &visualization)
{
    bound_visualization = &visualization;
}

void unbind_lane_views_lanes_visualization_bridge(
    LanesVisualization const &visualization)
{
    if (bound_visualization == &visualization) {
        bound_visualization = nullptr;
    }
}
} // namespace iv
