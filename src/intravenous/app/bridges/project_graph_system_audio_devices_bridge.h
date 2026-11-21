#pragma once
#include <intravenous/bridge.h>
namespace iv {
class ProjectGraph;
class SystemAudioDevices;
IV_DECLARE_BRIDGE(project_graph_system_audio_devices_bridge, ProjectGraph, SystemAudioDevices);
} // namespace iv
