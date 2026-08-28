#include <intravenous/runtime/lanes_visualization_timeline_bridge.h>

#include <intravenous/runtime/lanes_visualization.h>
#include <intravenous/runtime/lanes_visualization_events.h>
#include <intravenous/runtime/timeline.h>

namespace iv {
IV_DEFINE_BRIDGE(lanes_visualization_timeline_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    lanes_visualization_timeline_bridge,
    iv_runtime_lanes_visualization_timeline_batch_requested_event,
    &Timeline::handle_lanes_visualization_timeline_batch)
IV_SUBSCRIBE_LINKER_EVENT(
    lanes_visualization_timeline_bridge,
    iv_runtime_lanes_visualization_lane_output_query_event,
    &Timeline::handle_lanes_visualization_lane_output_query)
IV_SUBSCRIBE_LINKER_EVENT(
    lanes_visualization_timeline_bridge,
    iv_runtime_lanes_visualization_lane_ui_state_query_event,
    &Timeline::handle_lanes_visualization_lane_ui_state_query)
} // namespace iv
