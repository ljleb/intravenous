#pragma once

#include <intravenous/bridge.h>

namespace iv {
class AudioDeviceLanes;
class ProjectPersistence;

IV_DECLARE_BRIDGE(
    project_persistence_audio_device_lanes_bridge,
    ProjectPersistence,
    AudioDeviceLanes);
} // namespace iv
