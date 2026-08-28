#pragma once

#include <intravenous/bridge.h>

namespace iv {
class AudioDeviceLanes;
class TasksRunner;

IV_DECLARE_BRIDGE(
    task_runner_audio_device_lanes_bridge,
    TasksRunner,
    AudioDeviceLanes);
} // namespace iv
