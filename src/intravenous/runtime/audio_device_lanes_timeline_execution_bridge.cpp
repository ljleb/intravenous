#include <intravenous/runtime/audio_device_lanes_timeline_execution_bridge.h>

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/audio_device_lanes_events.h>
#include <intravenous/runtime/timeline_execution.h>
#include <intravenous/runtime/timeline_execution_events.h>

namespace iv {
namespace {
TimelineExecution *bound_execution = nullptr;
AudioDeviceLanes *bound_audio_device_lanes = nullptr;

void handle_set_realtime_start_index(size_t start_index)
{
    if (bound_execution == nullptr) {
        return;
    }
    bound_execution->set_realtime_start_index(start_index);
}

void handle_seek(SeekRequest const &request)
{
    if (bound_audio_device_lanes != nullptr) {
        bound_audio_device_lanes->seek_realtime_start_index(request.sample_index);
    }
}

IV_SUBSCRIBE_LINKER_EVENT(
    SeekEvent,
    iv_runtime_seek_event,
    handle_seek);
IV_SUBSCRIBE_LINKER_EVENT(
    AudioDeviceLanesSetRealtimeStartIndexEvent,
    iv_runtime_audio_device_lanes_set_realtime_start_index_event,
    handle_set_realtime_start_index);
} // namespace

void bind_audio_device_lanes_timeline_execution_bridge(
    AudioDeviceLanes &audio_device_lanes,
    TimelineExecution &execution)
{
    bound_audio_device_lanes = &audio_device_lanes;
    bound_execution = &execution;
}

void unbind_audio_device_lanes_timeline_execution_bridge(
    AudioDeviceLanes const &audio_device_lanes,
    TimelineExecution const &execution)
{
    if (bound_execution == &execution) {
        bound_execution = nullptr;
    }
    if (bound_audio_device_lanes == &audio_device_lanes) {
        bound_audio_device_lanes = nullptr;
    }
}
} // namespace iv
