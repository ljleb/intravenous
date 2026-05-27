#pragma once

#include "linker_event.h"
#include "runtime/graph_input_lane_controller.h"
#include "runtime/lane_ref.h"
#include "runtime/lane_view_service.h"

#include <optional>
#include <string>
#include <vector>

namespace iv {
struct GraphInputLanesPortsChangedRequest {
    std::vector<GraphInputPortDescriptor> ports;
};

struct GraphInputLanesLiveInputSnapshotRequest {
    std::string logical_node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample fallback = Sample {0.0f};
};

struct GraphInputLanesLiveInputSnapshot {
    std::string logical_node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample current_value = Sample {0.0f};
    bool has_concrete_override = false;
};

struct GraphInputLanesLaneOutputs {
    LaneId lane;
    std::vector<LaneOutputConnection> outputs;
};

struct GraphInputLanesSetSampleInputValueRequest {
    std::string node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample value = Sample {0.0f};
    GraphInputPortDescriptor graph_input_port;
};

struct GraphInputLanesClearSampleInputValueOverrideRequest {
    std::string node_id;
    size_t member_ordinal = 0;
    size_t input_ordinal = 0;
    GraphInputPortDescriptor graph_input_port;
};

struct GraphInputLanesSampleInputLaneRefRequest {
    std::string logical_node_id {};
    std::optional<size_t> member_ordinal {};
    size_t input_ordinal = 0;
    std::string input_name {};
    Sample default_value = Sample {0.0f};
};

class GraphInputLanesAckBuilder {
    std::optional<std::string> error_message;
    bool handled = false;

public:
    void succeed();
    void fail(std::string message);
    void build() const;
};

class GraphInputLanesLiveInputSnapshotsBuilder {
    std::optional<std::vector<GraphInputLanesLiveInputSnapshot>> result;

public:
    void succeed(std::vector<GraphInputLanesLiveInputSnapshot> value);
    [[nodiscard]] std::vector<GraphInputLanesLiveInputSnapshot> build() const;
};

class GraphInputLanesLaneBindingsBuilder {
    std::optional<GraphInputLaneBindings> result;

public:
    void succeed(GraphInputLaneBindings value);
    [[nodiscard]] GraphInputLaneBindings build() const;
};

class GraphInputLanesLaneOutputsBuilder {
    std::optional<std::vector<GraphInputLanesLaneOutputs>> result;

public:
    void succeed(std::vector<GraphInputLanesLaneOutputs> value);
    [[nodiscard]] std::vector<GraphInputLanesLaneOutputs> build() const;
};

class GraphInputLanesSampleInputLaneRefBuilder {
    std::optional<RealtimeLaneRef> result;

public:
    void succeed(RealtimeLaneRef value);
    [[nodiscard]] RealtimeLaneRef build() const;
};

using GraphInputLanesPortsChangedEvent =
    void (*)(GraphInputLanesPortsChangedRequest const &, GraphInputLanesAckBuilder &);
using GraphInputLanesLiveInputSnapshotsRequestedEvent =
    void (*)(std::vector<GraphInputLanesLiveInputSnapshotRequest> const &, GraphInputLanesLiveInputSnapshotsBuilder &);
using GraphInputLanesLaneBindingsEnsuredEvent =
    void (*)(GraphInputLanesPortsChangedRequest const &, GraphInputLanesAckBuilder &);
using GraphInputLanesLaneBindingsRequestedEvent =
    void (*)(GraphInputLanesPortsChangedRequest const &, GraphInputLanesLaneBindingsBuilder &);
using GraphInputLanesLaneOutputsRequestedEvent =
    void (*)(std::vector<LaneId> const &, GraphInputLanesLaneOutputsBuilder &);
using GraphInputLanesSetSampleInputValueRequestedEvent =
    void (*)(GraphInputLanesSetSampleInputValueRequest const &, GraphInputLanesAckBuilder &);
using GraphInputLanesClearSampleInputValueOverrideRequestedEvent =
    void (*)(GraphInputLanesClearSampleInputValueOverrideRequest const &, GraphInputLanesAckBuilder &);
using GraphInputLanesSampleInputLaneRefRequestedEvent =
    void (*)(GraphInputLanesSampleInputLaneRefRequest const &, GraphInputLanesSampleInputLaneRefBuilder &);
using GraphInputLanesLaneViewUpdatedEvent =
    void (*)(LaneViewResult const &);

IV_DECLARE_LINKER_EVENT(
    GraphInputLanesPortsChangedEvent,
    iv_runtime_graph_input_lanes_ports_changed_event);
IV_DECLARE_LINKER_EVENT(
    GraphInputLanesLiveInputSnapshotsRequestedEvent,
    iv_runtime_graph_input_lanes_live_input_snapshots_requested_event);
IV_DECLARE_LINKER_EVENT(
    GraphInputLanesLaneBindingsEnsuredEvent,
    iv_runtime_graph_input_lanes_lane_bindings_ensured_event);
IV_DECLARE_LINKER_EVENT(
    GraphInputLanesLaneBindingsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_bindings_requested_event);
IV_DECLARE_LINKER_EVENT(
    GraphInputLanesLaneOutputsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_outputs_requested_event);
IV_DECLARE_LINKER_EVENT(
    GraphInputLanesSetSampleInputValueRequestedEvent,
    iv_runtime_graph_input_lanes_set_sample_input_value_requested_event);
IV_DECLARE_LINKER_EVENT(
    GraphInputLanesClearSampleInputValueOverrideRequestedEvent,
    iv_runtime_graph_input_lanes_clear_sample_input_value_override_requested_event);
IV_DECLARE_LINKER_EVENT(
    GraphInputLanesSampleInputLaneRefRequestedEvent,
    iv_runtime_graph_input_lanes_sample_input_lane_ref_requested_event);
IV_DECLARE_LINKER_EVENT(
    GraphInputLanesLaneViewUpdatedEvent,
    iv_runtime_graph_input_lanes_lane_view_updated_event);
} // namespace iv
