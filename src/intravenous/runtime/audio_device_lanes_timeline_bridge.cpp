#include <intravenous/runtime/audio_device_lanes_timeline_bridge.h>

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/audio_device_lanes_events.h>
#include <intravenous/runtime/timeline.h>

namespace iv {
IV_DEFINE_BRIDGE(audio_device_lanes_timeline_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    audio_device_lanes_timeline_bridge,
    iv_runtime_audio_device_lanes_timeline_batch_requested_event,
    &Timeline::handle_audio_device_lanes_timeline_batch)
} // namespace iv
