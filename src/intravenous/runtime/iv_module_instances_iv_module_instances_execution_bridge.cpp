#include <intravenous/runtime/iv_module_instances_iv_module_instances_execution_bridge.h>

#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_execution_events.h>
#include <intravenous/runtime/audio_device_lanes_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/timeline_execution_events.h>

namespace iv {
namespace {
IvModuleInstancesExecution *bound_execution = nullptr;

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
        "iv module execution tasks changed: revision="
        + std::to_string(update.version_index)
        + " create=" + std::to_string(update.update.to_create.size())
        + " update=" + std::to_string(update.update.to_update.size())
        + " delete=" + std::to_string(update.update.to_delete.size()));
    IV_INVOKE_LINKER_EVENT(iv_runtime_iv_module_instances_execution_tasks_changed_event, update);
}

void handle_iv_module_instance_builders_completed(
    IvModuleInstanceBuildersChanged const &changed)
{
    if (!bound_execution) {
        return;
    }
    maybe_publish(bound_execution->handle_instance_builders_changed(changed));
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleInstanceBuildersCompletedEvent,
    iv_runtime_iv_module_instance_builders_completed_event,
    handle_iv_module_instance_builders_completed);
IV_SUBSCRIBE_LINKER_EVENT(
    AudioDeviceLanesSetRealtimeStartIndexEvent,
    iv_runtime_audio_device_lanes_set_realtime_start_index_event,
    +[](size_t start_index) {
        if (bound_execution) {
            // During transport playback, module DSP blocks share the audio
            // device's timeline origin. When transport is paused this event
            // stops, so module preview execution keeps its own advancing
            // playhead.
            bound_execution->synchronize_transport_playhead(start_index);
        }
    });
IV_SUBSCRIBE_LINKER_EVENT(
    PauseEvent,
    iv_runtime_pause_event,
    +[](PauseRequest const &) {
        if (bound_execution) {
            bound_execution->set_follows_transport_playhead(false);
        }
    });
IV_SUBSCRIBE_LINKER_EVENT(
    ResumeEvent,
    iv_runtime_resume_event,
    +[](ResumeRequest const &) {
        if (bound_execution) {
            bound_execution->set_follows_transport_playhead(true);
        }
    });
IV_SUBSCRIBE_LINKER_EVENT(
    TimelineExecutionResumedEvent,
    iv_runtime_timeline_execution_resumed_event,
    +[](TimelineExecutionResumed const &resumed) {
        if (!bound_execution) {
            return;
        }
        bound_execution->resume(resumed.start_index);
    });
}

void bind_iv_module_instances_iv_module_instances_execution_bridge(
    IvModuleInstancesExecution &execution)
{
    bound_execution = &execution;
}

void unbind_iv_module_instances_iv_module_instances_execution_bridge(
    IvModuleInstancesExecution const &execution)
{
    if (bound_execution == &execution) {
        bound_execution = nullptr;
    }
}
}
