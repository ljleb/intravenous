#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/runtime/timeline_events.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iv {
struct GraphInputLanesRuntimeDependency {
    std::string instance_id {};
    std::vector<LaneId> prerequisite_lanes {};
};

struct GraphInputLanesRuntimeDependenciesChanged {
    std::uint64_t version_index = 0;
    std::vector<GraphInputLanesRuntimeDependency> instances {};
};

class GraphInputLanesAckBuilder {
    std::optional<std::string> error_message;
    bool handled = false;

public:
    void succeed();
    void fail(std::string message);
    void build() const;
};

class GraphInputLanesSampleBlockBuilder {
    BorrowedSampleBlock block_ {};

public:
    void succeed(BorrowedSampleBlock block)
    {
        block_ = block;
    }

    [[nodiscard]] BorrowedSampleBlock build() const
    {
        return block_;
    }
};

class GraphInputLanesEventBlockBuilder {
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

using GraphInputLanesTimelineBatchRequestedEvent =
    void (*)(TimelineLaneBatchUpdate const &, GraphInputLanesAckBuilder &);
using GraphInputLanesRuntimeDependenciesChangedEvent =
    void (*)(GraphInputLanesRuntimeDependenciesChanged const &);

// A knob's value is mutable DSP state.  Updating it must not change graph
// topology or request an IV module rebuild.
using GraphInputLanesKnobValueUpdatedEvent = void (*)(LaneId, Sample);
using GraphInputLanesSampleBlockPublishedEvent =
    void (*)(LaneId, BorrowedSampleBlock const &);
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
    GraphInputLanesRuntimeDependenciesChangedEvent,
    iv_runtime_graph_input_lanes_runtime_dependencies_changed_event);
IV_DECLARE_SINGLETON_EVENT(
    GraphInputLanesKnobValueUpdatedEvent,
    iv_runtime_graph_input_lanes_knob_value_updated_event);
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
