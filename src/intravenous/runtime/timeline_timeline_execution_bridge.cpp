#include <intravenous/runtime/timeline_timeline_execution_bridge.h>

#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/timeline_execution.h>
#include <intravenous/runtime/timeline_execution_events.h>

namespace iv {
namespace {
Timeline *bound_timeline = nullptr;
TimelineExecution *bound_execution = nullptr;

void maybe_publish(VersionedTaskGraphUpdate const &update)
{
    if (update.update.to_create.empty()
        && update.update.to_update.empty()
        && update.update.to_delete.empty()) {
        return;
    }
    IV_INVOKE_LINKER_EVENT(iv_runtime_timeline_execution_tasks_changed_event, update);
}

void handle_timeline_lanes_changed(TimelineLanesChanged const &changed)
{
    if (!bound_execution) {
        return;
    }
    maybe_publish(bound_execution->handle_timeline_lanes_changed(changed));
}

void handle_realtime_sample_block_requested(
    LaneId lane,
    TimelineExecutionRealtimeSampleBlockBuilder &builder)
{
    if (!bound_execution) {
        return;
    }
    builder.succeed(bound_execution->realtime_sample_block(lane));
}

void handle_realtime_event_block_requested(
    LaneId lane,
    TimelineExecutionRealtimeEventBlockBuilder &builder)
{
    if (!bound_execution) {
        return;
    }
    builder.succeed(bound_execution->realtime_event_block(lane));
}

IV_SUBSCRIBE_LINKER_EVENT(
    TimelineLanesChangedEvent,
    iv_runtime_timeline_lanes_changed_event,
    handle_timeline_lanes_changed);
IV_SUBSCRIBE_SINGLETON_EVENT(
    TimelineExecutionRealtimeSampleBlockRequestedEvent,
    iv_runtime_timeline_execution_realtime_sample_block_requested_event,
    handle_realtime_sample_block_requested);
IV_SUBSCRIBE_SINGLETON_EVENT(
    TimelineExecutionRealtimeEventBlockRequestedEvent,
    iv_runtime_timeline_execution_realtime_event_block_requested_event,
    handle_realtime_event_block_requested);
}

void bind_timeline_timeline_execution_bridge(Timeline &timeline, TimelineExecution &execution)
{
    bound_timeline = &timeline;
    bound_execution = &execution;
    auto update = timeline.with_graph([&](LaneGraph const &graph) {
        return execution.synchronize_from_graph(graph);
    });
    maybe_publish(update);
}

void unbind_timeline_timeline_execution_bridge(Timeline const &timeline, TimelineExecution const &execution)
{
    if (bound_timeline == &timeline) {
        bound_timeline = nullptr;
    }
    if (bound_execution == &execution) {
        bound_execution = nullptr;
    }
}
}
