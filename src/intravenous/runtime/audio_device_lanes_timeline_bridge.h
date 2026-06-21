#pragma once

namespace iv {
class AudioDeviceLanes;
class Timeline;

void bind_audio_device_lanes_timeline_bridge(AudioDeviceLanes &audio_device_lanes, Timeline &timeline);
void unbind_audio_device_lanes_timeline_bridge(
    AudioDeviceLanes const &audio_device_lanes,
    Timeline const &timeline);
} // namespace iv
