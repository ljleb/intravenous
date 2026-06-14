#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_graph.h>

#include <span>

namespace iv {
class GraphOutputBlocksSampleBlockBuilder {
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

class GraphOutputBlocksEventBlockBuilder {
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

using GraphOutputBlocksSamplePublishedEvent =
    void (*)(LaneId, std::span<Sample const>);
using GraphOutputBlocksEventPublishedEvent =
    void (*)(LaneId, std::span<TimedEvent const>);
using GraphOutputBlocksSampleRequestedEvent =
    void (*)(LaneId, GraphOutputBlocksSampleBlockBuilder &);
using GraphOutputBlocksEventRequestedEvent =
    void (*)(LaneId, GraphOutputBlocksEventBlockBuilder &);

IV_DECLARE_SINGLETON_EVENT(
    GraphOutputBlocksSamplePublishedEvent,
    iv_runtime_graph_output_blocks_sample_published_event);
IV_DECLARE_SINGLETON_EVENT(
    GraphOutputBlocksEventPublishedEvent,
    iv_runtime_graph_output_blocks_event_published_event);
IV_DECLARE_SINGLETON_EVENT(
    GraphOutputBlocksSampleRequestedEvent,
    iv_runtime_graph_output_blocks_sample_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    GraphOutputBlocksEventRequestedEvent,
    iv_runtime_graph_output_blocks_event_requested_event);
}
