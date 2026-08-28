#include <intravenous/runtime/lane_views_lanes_visualization_bridge.h>

#include <intravenous/runtime/lane_views_events.h>
#include <intravenous/runtime/lanes_visualization.h>

namespace iv {
IV_DEFINE_BRIDGE(lane_views_lanes_visualization_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    lane_views_lanes_visualization_bridge,
    iv_runtime_lane_views_updated_event,
    &LanesVisualization::handle_lane_views_updated);
IV_SUBSCRIBE_LINKER_EVENT(
    lane_views_lanes_visualization_bridge,
    iv_runtime_lane_view_closed_event,
    static_cast<void (LanesVisualization::*)(std::string const &)>(
        &LanesVisualization::handle_lane_view_closed));
} // namespace iv
