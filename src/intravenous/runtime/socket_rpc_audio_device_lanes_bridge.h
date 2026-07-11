#pragma once

namespace iv {
class AudioDeviceLanes;

void bind_socket_rpc_audio_device_lanes_bridge(AudioDeviceLanes &audio_device_lanes);
void unbind_socket_rpc_audio_device_lanes_bridge(
    AudioDeviceLanes const &audio_device_lanes);
} // namespace iv
