#pragma once

#include <intravenous/bridge.h>

namespace iv {
class AudioDeviceLanes;
class Timeline;

IV_DECLARE_BRIDGE(
    audio_device_lanes_timeline_bridge,
    AudioDeviceLanes,
    Timeline);
} // namespace iv
