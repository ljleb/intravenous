#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/task_runner.h>

#include <span>

namespace iv {
using TimelineExecutionTasksChangedEvent =
    void (*)(TaskGraphUpdate const &);

class TimelineExecutionRealtimeSampleBlockBuilder {
    std::span<Sample const> block_ {};

public:
    void succeed(std::span<Sample const> block)
    {
        block_ = block;
    }

    [[nodiscard]] std::span<Sample const> build() const
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
IV_DECLARE_SINGLETON_EVENT(
    TimelineExecutionRealtimeSampleBlockRequestedEvent,
    iv_runtime_timeline_execution_realtime_sample_block_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    TimelineExecutionRealtimeEventBlockRequestedEvent,
    iv_runtime_timeline_execution_realtime_event_block_requested_event);
}
