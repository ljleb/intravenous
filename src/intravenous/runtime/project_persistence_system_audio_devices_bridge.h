#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ProjectPersistence;
class SystemAudioDevices;

IV_DECLARE_BRIDGE(
    project_persistence_system_audio_devices_bridge,
    ProjectPersistence,
    SystemAudioDevices);
} // namespace iv
