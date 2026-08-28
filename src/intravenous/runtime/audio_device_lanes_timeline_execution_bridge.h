#pragma once

#include <intravenous/bridge.h>

namespace iv {
class AudioDeviceLanes;
class TimelineExecution;

IV_DECLARE_BRIDGE(
    audio_device_lanes_timeline_execution_bridge,
    AudioDeviceLanes,
    TimelineExecution);
} // namespace iv
