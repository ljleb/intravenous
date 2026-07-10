#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/runtime/task_runner_events.h>
#include <intravenous/runtime/timeline_events.h>

namespace iv {

class AudioDeviceLanesInputBlockBuilder {
    OwnedSampleBlock block_ {};

public:
    void succeed(OwnedSampleBlock block)
    {
        block_ = std::move(block);
    }

    [[nodiscard]] OwnedSampleBlock build() const
    {
        return block_;
    }
};

using AudioDeviceLanesTimelineBatchRequestedEvent =
    void (*)(TimelineLaneBatchUpdate const &);
using AudioDeviceLanesInputBlockRequestedEvent =
    void (*)(LaneId, AudioDeviceLanesInputBlockBuilder &);
using AudioDeviceLanesSetRealtimeStartIndexEvent =
    void (*)(size_t);

IV_DECLARE_LINKER_EVENT(
    AudioDeviceLanesTimelineBatchRequestedEvent,
    iv_runtime_audio_device_lanes_timeline_batch_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    AudioDeviceLanesInputBlockRequestedEvent,
    iv_runtime_audio_device_lanes_input_block_requested_event);
IV_DECLARE_LINKER_EVENT(
    AudioDeviceLanesSetRealtimeStartIndexEvent,
    iv_runtime_audio_device_lanes_set_realtime_start_index_event);

} // namespace iv
