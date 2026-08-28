#include <intravenous/runtime/task_runner_audio_device_lanes_bridge.h>

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/task_runner_events.h>

namespace iv {
IV_DEFINE_BRIDGE(task_runner_audio_device_lanes_bridge)
IV_SUBSCRIBE_BRIDGE(
    task_runner_audio_device_lanes_bridge,
    iv_runtime_task_runner_before_pass_event,
    &AudioDeviceLanes::handle_task_runner_before_pass);
IV_SUBSCRIBE_BRIDGE(
    task_runner_audio_device_lanes_bridge,
    iv_runtime_task_runner_after_pass_event,
    &AudioDeviceLanes::handle_task_runner_after_pass);
} // namespace iv
