#pragma once

namespace iv {
class AudioDeviceLanes;
class TimelineExecution;

void bind_audio_device_lanes_timeline_execution_bridge(
    AudioDeviceLanes &audio_device_lanes,
    TimelineExecution &execution);
void unbind_audio_device_lanes_timeline_execution_bridge(
    AudioDeviceLanes const &audio_device_lanes,
    TimelineExecution const &execution);
} // namespace iv
