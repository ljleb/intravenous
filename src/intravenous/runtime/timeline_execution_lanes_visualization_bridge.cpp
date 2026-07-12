#include <intravenous/runtime/timeline_execution_lanes_visualization_bridge.h>

#include <intravenous/runtime/lanes_visualization_events.h>
#include <intravenous/runtime/timeline_execution.h>

namespace iv {
namespace {
TimelineExecution *bound_execution = nullptr;

void handle_playback_position_query(LanesVisualizationPlaybackPositionBuilder &builder)
{
    if (bound_execution != nullptr) {
        builder.succeed(bound_execution->realtime_start_index());
    }
}

void handle_compiled_sample_level_requested(
    LaneId lane,
    size_t first,
    size_t last,
    LanesVisualizationCompiledSampleLevelBuilder &builder)
{
    if (bound_execution == nullptr) {
        return;
    }
    builder.succeed(bound_execution->compiled_sample_level(lane, first, last));
}

void handle_compiled_event_window_requested(
    LaneId lane,
    size_t first,
    size_t last,
    LanesVisualizationCompiledEventWindowBuilder &builder)
{
    if (bound_execution == nullptr) {
        return;
    }
    builder.succeed(bound_execution->compiled_events_in_range(lane, first, last));
}

IV_SUBSCRIBE_LINKER_EVENT(
    LanesVisualizationPlaybackPositionQueryEvent,
    iv_runtime_lanes_visualization_playback_position_query_event,
    handle_playback_position_query);
IV_SUBSCRIBE_LINKER_EVENT(
    LanesVisualizationCompiledSampleLevelRequestedEvent,
    iv_runtime_lanes_visualization_compiled_sample_level_requested_event,
    handle_compiled_sample_level_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    LanesVisualizationCompiledEventWindowRequestedEvent,
    iv_runtime_lanes_visualization_compiled_event_window_requested_event,
    handle_compiled_event_window_requested);
} // namespace

void bind_timeline_execution_lanes_visualization_bridge(TimelineExecution &execution)
{
    bound_execution = &execution;
}

void unbind_timeline_execution_lanes_visualization_bridge(
    TimelineExecution const &execution)
{
    if (bound_execution == &execution) {
        bound_execution = nullptr;
    }
}
} // namespace iv
