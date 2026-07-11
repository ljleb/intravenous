#include <intravenous/runtime/task_runner_audio_device_lanes_bridge.h>

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/task_runner_events.h>

namespace iv {
namespace {
AudioDeviceLanes *bound_audio_device_lanes = nullptr;

void handle_task_runner_before_pass(TasksRunnerBeforePass const &pass)
{
    if (bound_audio_device_lanes == nullptr) {
        return;
    }
    bound_audio_device_lanes->handle_task_runner_before_pass(pass);
}

void handle_task_runner_after_pass(TasksRunnerAfterPass const &pass)
{
    if (bound_audio_device_lanes == nullptr) {
        return;
    }
    bound_audio_device_lanes->handle_task_runner_after_pass(pass);
}

IV_SUBSCRIBE_LINKER_EVENT(
    TasksRunnerBeforePassEvent,
    iv_runtime_task_runner_before_pass_event,
    handle_task_runner_before_pass);
IV_SUBSCRIBE_LINKER_EVENT(
    TasksRunnerAfterPassEvent,
    iv_runtime_task_runner_after_pass_event,
    handle_task_runner_after_pass);
} // namespace

void bind_task_runner_audio_device_lanes_bridge(AudioDeviceLanes &audio_device_lanes)
{
    bound_audio_device_lanes = &audio_device_lanes;
}

void unbind_task_runner_audio_device_lanes_bridge(AudioDeviceLanes const &audio_device_lanes)
{
    if (bound_audio_device_lanes == &audio_device_lanes) {
        bound_audio_device_lanes = nullptr;
    }
}
} // namespace iv
