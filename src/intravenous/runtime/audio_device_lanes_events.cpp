#include <intravenous/runtime/audio_device_lanes_events.h>

namespace iv {
namespace {
void default_input_block_requested(LaneId, AudioDeviceLanesInputBlockBuilder &)
{
}
}

IV_DEFINE_LINKER_EVENT(
    AudioDeviceLanesTimelineBatchRequestedEvent,
    iv_runtime_audio_device_lanes_timeline_batch_requested_event);
IV_DEFINE_SINGLETON_EVENT(
    AudioDeviceLanesInputBlockRequestedEvent,
    iv_runtime_audio_device_lanes_input_block_requested_event,
    default_input_block_requested);
IV_DEFINE_LINKER_EVENT(
    AudioDeviceLanesSetRealtimeStartIndexEvent,
    iv_runtime_audio_device_lanes_set_realtime_start_index_event);
} // namespace iv
