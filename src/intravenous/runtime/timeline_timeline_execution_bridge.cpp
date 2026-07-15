#include <intravenous/runtime/timeline_timeline_execution_bridge.h>

#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/timeline_execution.h>
#include <intravenous/runtime/timeline_execution_events.h>

namespace iv {
namespace {
Timeline *bound_timeline = nullptr;
TimelineExecution *bound_execution = nullptr;

void emit_debug_message(std::string message)
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectMessageNotification{
            .level = "debug",
            .message = std::move(message),
        }));
}

void maybe_publish(VersionedTaskGraphUpdate const &update)
{
    if (update.update.to_create.empty()
        && update.update.to_update.empty()
        && update.update.to_delete.empty()) {
        return;
    }
    emit_debug_message(
        "timeline execution tasks changed: revision="
        + std::to_string(update.version_index)
        + " create=" + std::to_string(update.update.to_create.size())
        + " update=" + std::to_string(update.update.to_update.size())
        + " delete=" + std::to_string(update.update.to_delete.size()));
    IV_INVOKE_LINKER_EVENT(iv_runtime_timeline_execution_tasks_changed_event, update);
}

void handle_timeline_lanes_changed(TimelineLanesChanged const &changed)
{
    if (!bound_execution) {
        return;
    }
    auto const update = bound_execution->handle_timeline_lanes_changed(changed);
    emit_debug_message(
        "lane topology diagnostic: execution applied lane change version="
        + std::to_string(changed.version_index)
        + " changed=" + std::to_string(changed.changed_lanes.size())
        + " created=" + std::to_string(changed.created_lanes.size())
        + " removed=" + std::to_string(changed.removed_lanes.size())
        + " taskUpdates=" + std::to_string(update.update.to_update.size()));
    maybe_publish(update);
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

void handle_pause(PauseRequest const &)
{
    if (!bound_execution) {
        return;
    }
    bound_execution->pause();
    bound_execution->seek(bound_execution->last_scrubbed_index());
    emit_debug_message("timeline paused");
}

void handle_resume(ResumeRequest const &request)
{
    if (!bound_execution) {
        return;
    }
    auto const start_index = request.start_index;
    bound_execution->resume(start_index);
    emit_debug_message(
        "timeline resumed: startIndex=" + std::to_string(start_index));
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_timeline_execution_resumed_event,
        TimelineExecutionResumed{
            .start_index = start_index,
        });
}

void handle_seek(SeekRequest const &request)
{
    if (!bound_execution) {
        return;
    }
    bool const was_paused = bound_execution->is_paused();
    bound_execution->seek(request.sample_index);
    if (!was_paused) {
        // While transport is running, the DSP graph follows its timeline
        // origin. A paused transport deliberately leaves module preview
        // execution on its independent playhead until the next resume.
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_timeline_execution_resumed_event,
            TimelineExecutionResumed{ .start_index = request.sample_index });
    }
}

IV_SUBSCRIBE_LINKER_EVENT(
    SeekEvent,
    iv_runtime_seek_event,
    handle_seek);
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
IV_SUBSCRIBE_LINKER_EVENT(
    PauseEvent,
    iv_runtime_pause_event,
    handle_pause);
IV_SUBSCRIBE_LINKER_EVENT(
    ResumeEvent,
    iv_runtime_resume_event,
    handle_resume);
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
