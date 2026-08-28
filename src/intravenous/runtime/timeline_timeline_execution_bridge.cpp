#include <intravenous/runtime/timeline_timeline_execution_bridge.h>

#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/timeline_execution.h>
#include <intravenous/runtime/timeline_execution_events.h>

namespace iv {
IV_DEFINE_BRIDGE(timeline_timeline_execution_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    timeline_timeline_execution_bridge,
    iv_runtime_timeline_lanes_changed_event,
    &TimelineExecution::handle_timeline_lanes_changed_event)
IV_SUBSCRIBE_LINKER_EVENT(
    timeline_timeline_execution_bridge,
    iv_runtime_seek_event,
    &TimelineExecution::handle_seek)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    timeline_timeline_execution_bridge,
    iv_runtime_timeline_execution_realtime_sample_block_requested_event,
    &TimelineExecution::handle_timeline_execution_realtime_sample_block_requested)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    timeline_timeline_execution_bridge,
    iv_runtime_timeline_execution_realtime_event_block_requested_event,
    &TimelineExecution::handle_timeline_execution_realtime_event_block_requested)
IV_SUBSCRIBE_LINKER_EVENT(
    timeline_timeline_execution_bridge,
    iv_runtime_pause_event,
    &TimelineExecution::handle_pause)
IV_SUBSCRIBE_LINKER_EVENT(
    timeline_timeline_execution_bridge,
    iv_runtime_resume_event,
    &TimelineExecution::handle_resume)
IV_SUBSCRIBE_LINKER_EVENT(
    timeline_timeline_execution_bridge,
    iv_runtime_timeline_execution_invalidate_compiled_cache_event,
    &TimelineExecution::invalidate_compiled_cache)
} // namespace iv
