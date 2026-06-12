#include <intravenous/runtime/timeline_execution_events.h>

namespace iv {
namespace {
void default_realtime_sample_block_requested(
    LaneId,
    TimelineExecutionRealtimeSampleBlockBuilder &)
{}

void default_realtime_event_block_requested(
    LaneId,
    TimelineExecutionRealtimeEventBlockBuilder &)
{}
}

IV_DEFINE_LINKER_EVENT(
    TimelineExecutionTasksChangedEvent,
    iv_runtime_timeline_execution_tasks_changed_event);
IV_DEFINE_SINGLETON_EVENT(
    TimelineExecutionRealtimeSampleBlockRequestedEvent,
    iv_runtime_timeline_execution_realtime_sample_block_requested_event,
    default_realtime_sample_block_requested);
IV_DEFINE_SINGLETON_EVENT(
    TimelineExecutionRealtimeEventBlockRequestedEvent,
    iv_runtime_timeline_execution_realtime_event_block_requested_event,
    default_realtime_event_block_requested);
}
