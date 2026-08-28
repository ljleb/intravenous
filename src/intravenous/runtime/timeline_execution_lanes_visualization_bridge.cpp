#include <intravenous/runtime/timeline_execution_lanes_visualization_bridge.h>

#include <intravenous/runtime/lanes_visualization_events.h>
#include <intravenous/runtime/timeline_execution.h>

namespace iv {
IV_DEFINE_BRIDGE(timeline_execution_lanes_visualization_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    timeline_execution_lanes_visualization_bridge,
    iv_runtime_lanes_visualization_playback_position_query_event,
    &TimelineExecution::handle_lanes_visualization_playback_position_query);
IV_SUBSCRIBE_LINKER_EVENT(
    timeline_execution_lanes_visualization_bridge,
    iv_runtime_lanes_visualization_compiled_sample_window_requested_event,
    &TimelineExecution::handle_lanes_visualization_compiled_sample_window_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    timeline_execution_lanes_visualization_bridge,
    iv_runtime_lanes_visualization_compiled_event_window_requested_event,
    &TimelineExecution::handle_lanes_visualization_compiled_event_window_requested);
} // namespace iv
