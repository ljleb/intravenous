#include <intravenous/runtime/audio_device_lanes_timeline_execution_bridge.h>

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/audio_device_lanes_events.h>
#include <intravenous/runtime/timeline_execution.h>
#include <intravenous/runtime/timeline_execution_events.h>

namespace iv {
IV_DEFINE_BRIDGE(audio_device_lanes_timeline_execution_bridge)
IV_SUBSCRIBE_BRIDGE(
    audio_device_lanes_timeline_execution_bridge,
    iv_runtime_timeline_execution_resumed_event,
    &AudioDeviceLanes::handle_timeline_execution_resumed);
IV_SUBSCRIBE_BRIDGE(
    audio_device_lanes_timeline_execution_bridge,
    iv_runtime_seek_event,
    &AudioDeviceLanes::handle_seek);
IV_SUBSCRIBE_BRIDGE(
    audio_device_lanes_timeline_execution_bridge,
    iv_runtime_audio_device_lanes_set_realtime_start_index_event,
    &TimelineExecution::set_realtime_start_index);
} // namespace iv
