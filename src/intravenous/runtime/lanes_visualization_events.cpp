#include <intravenous/runtime/lanes_visualization_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    LanesVisualizationLaneOutputQueryEvent,
    iv_runtime_lanes_visualization_lane_output_query_event);
IV_DEFINE_LINKER_EVENT(
    LanesVisualizationPlaybackPositionQueryEvent,
    iv_runtime_lanes_visualization_playback_position_query_event);
IV_DEFINE_LINKER_EVENT(
    LanesVisualizationLaneUiStateQueryEvent,
    iv_runtime_lanes_visualization_lane_ui_state_query_event);
IV_DEFINE_LINKER_EVENT(
    LanesVisualizationCompiledSampleLevelRequestedEvent,
    iv_runtime_lanes_visualization_compiled_sample_level_requested_event);
IV_DEFINE_LINKER_EVENT(
    LanesVisualizationCompiledEventWindowRequestedEvent,
    iv_runtime_lanes_visualization_compiled_event_window_requested_event);
IV_DEFINE_LINKER_EVENT(
    LanesVisualizationTimelineBatchRequestedEvent,
    iv_runtime_lanes_visualization_timeline_batch_requested_event);
IV_DEFINE_LINKER_EVENT(
    LaneViewContentUpdatedEvent,
    iv_runtime_lane_view_content_updated_event);
} // namespace iv
