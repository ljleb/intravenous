#include <intravenous/runtime/audio_device_lanes_timeline_execution_bridge.h>

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/audio_device_lanes_events.h>
#include <intravenous/runtime/timeline_execution.h>

namespace iv {
namespace {
TimelineExecution *bound_execution = nullptr;

void handle_set_realtime_start_index(size_t start_index)
{
    if (bound_execution == nullptr) {
        return;
    }
    bound_execution->set_realtime_start_index(start_index);
}

IV_SUBSCRIBE_LINKER_EVENT(
    AudioDeviceLanesSetRealtimeStartIndexEvent,
    iv_runtime_audio_device_lanes_set_realtime_start_index_event,
    handle_set_realtime_start_index);
} // namespace

void bind_audio_device_lanes_timeline_execution_bridge(
    AudioDeviceLanes &,
    TimelineExecution &execution)
{
    bound_execution = &execution;
}

void unbind_audio_device_lanes_timeline_execution_bridge(
    AudioDeviceLanes const &,
    TimelineExecution const &execution)
{
    if (bound_execution == &execution) {
        bound_execution = nullptr;
    }
}
} // namespace iv
