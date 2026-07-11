#include <intravenous/runtime/timeline_execution_lanes_visualization_bridge.h>

#include <intravenous/runtime/lanes_visualization_events.h>
#include <intravenous/runtime/timeline_execution.h>

namespace iv {
namespace {
TimelineExecution *bound_execution = nullptr;

void handle_compiled_sample_window_requested(
    LaneId lane,
    size_t first,
    size_t last,
    size_t count,
    LanesVisualizationCompiledSampleWindowBuilder &builder)
{
    if (bound_execution == nullptr) {
        return;
    }
    builder.succeed(bound_execution->sparse_compiled_sample_window(lane, first, last, count));
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
    LanesVisualizationCompiledSampleWindowRequestedEvent,
    iv_runtime_lanes_visualization_compiled_sample_window_requested_event,
    handle_compiled_sample_window_requested);
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
