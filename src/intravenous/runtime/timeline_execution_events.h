#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/runtime/task_runner.h>

#include <cstddef>

namespace iv {
struct PauseRequest {};

struct ResumeRequest {
    size_t start_index = 0;
};

struct TimelineExecutionResumed {
    size_t start_index = 0;
};

using TimelineExecutionTasksChangedEvent =
    void (*)(VersionedTaskGraphUpdate const &);
using PauseEvent =
    void (*)(PauseRequest const &);
using ResumeEvent =
    void (*)(ResumeRequest const &);
using TimelineExecutionResumedEvent =
    void (*)(TimelineExecutionResumed const &);

class TimelineExecutionRealtimeSampleBlockBuilder {
    BorrowedSampleBlock block_ {};

public:
    void succeed(BorrowedSampleBlock block)
    {
        block_ = std::move(block);
    }

    [[nodiscard]] BorrowedSampleBlock build() const
    {
        return block_;
    }
};

class TimelineExecutionRealtimeEventBlockBuilder {
    std::span<TimedEvent const> events_ {};

public:
    void succeed(std::span<TimedEvent const> events)
    {
        events_ = events;
    }

    [[nodiscard]] std::span<TimedEvent const> build() const
    {
        return events_;
    }
};

using TimelineExecutionRealtimeSampleBlockRequestedEvent =
    void (*)(LaneId, TimelineExecutionRealtimeSampleBlockBuilder &);
using TimelineExecutionRealtimeEventBlockRequestedEvent =
    void (*)(LaneId, TimelineExecutionRealtimeEventBlockBuilder &);

IV_DECLARE_LINKER_EVENT(
    TimelineExecutionTasksChangedEvent,
    iv_runtime_timeline_execution_tasks_changed_event);
IV_DECLARE_LINKER_EVENT(
    PauseEvent,
    iv_runtime_pause_event);
IV_DECLARE_LINKER_EVENT(
    ResumeEvent,
    iv_runtime_resume_event);
IV_DECLARE_LINKER_EVENT(
    TimelineExecutionResumedEvent,
    iv_runtime_timeline_execution_resumed_event);
IV_DECLARE_SINGLETON_EVENT(
    TimelineExecutionRealtimeSampleBlockRequestedEvent,
    iv_runtime_timeline_execution_realtime_sample_block_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    TimelineExecutionRealtimeEventBlockRequestedEvent,
    iv_runtime_timeline_execution_realtime_event_block_requested_event);
}
