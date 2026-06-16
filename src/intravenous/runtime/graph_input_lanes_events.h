#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/timeline_events.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iv {
class GraphInputLanesAckBuilder {
    std::optional<std::string> error_message;
    bool handled = false;

public:
    void succeed();
    void fail(std::string message);
    void build() const;
};

class GraphInputLanesSampleBlockBuilder {
    std::vector<Sample> block_ {};

public:
    void succeed(std::vector<Sample> block)
    {
        block_ = std::move(block);
    }

    [[nodiscard]] std::vector<Sample> build() const
    {
        return block_;
    }
};

class GraphInputLanesEventBlockBuilder {
    std::vector<TimedEvent> events_ {};

public:
    void succeed(std::vector<TimedEvent> events)
    {
        events_ = std::move(events);
    }

    [[nodiscard]] std::vector<TimedEvent> build() const
    {
        return events_;
    }
};

using GraphInputLanesTimelineBatchRequestedEvent =
    void (*)(TimelineLaneBatchUpdate const &, GraphInputLanesAckBuilder &);

struct GraphInputLanesRebuildRequested {
    std::uint64_t version_index = 0;
    std::vector<std::string> instance_ids {};
};

using GraphInputLanesRebuildRequestedEvent =
    void (*)(GraphInputLanesRebuildRequested const &);
using GraphInputLanesSampleBlockPublishedEvent =
    void (*)(LaneId, std::span<Sample const>);
using GraphInputLanesEventBlockPublishedEvent =
    void (*)(LaneId, std::span<TimedEvent const>);
using GraphInputLanesSampleBlockRequestedEvent =
    void (*)(LaneId, GraphInputLanesSampleBlockBuilder &);
using GraphInputLanesEventBlockRequestedEvent =
    void (*)(LaneId, GraphInputLanesEventBlockBuilder &);

IV_DECLARE_LINKER_EVENT(
    GraphInputLanesTimelineBatchRequestedEvent,
    iv_runtime_graph_input_lanes_timeline_batch_requested_event);
IV_DECLARE_LINKER_EVENT(
    GraphInputLanesRebuildRequestedEvent,
    iv_runtime_graph_input_lanes_rebuild_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    GraphInputLanesSampleBlockPublishedEvent,
    iv_runtime_graph_input_lanes_sample_block_published_event);
IV_DECLARE_SINGLETON_EVENT(
    GraphInputLanesEventBlockPublishedEvent,
    iv_runtime_graph_input_lanes_event_block_published_event);
IV_DECLARE_SINGLETON_EVENT(
    GraphInputLanesSampleBlockRequestedEvent,
    iv_runtime_graph_input_lanes_sample_block_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    GraphInputLanesEventBlockRequestedEvent,
    iv_runtime_graph_input_lanes_event_block_requested_event);
} // namespace iv
