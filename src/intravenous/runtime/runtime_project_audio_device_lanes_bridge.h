#pragma once

namespace iv {
class AudioDeviceLanes;

void bind_runtime_project_audio_device_lanes_bridge(AudioDeviceLanes &audio_device_lanes);
void unbind_runtime_project_audio_device_lanes_bridge(AudioDeviceLanes const &audio_device_lanes);
} // namespace iv
