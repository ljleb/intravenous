#pragma once

#include "linker_event.h"
#include "runtime/graph_input_lane_controller.h"
#include "runtime/lane_view_service.h"

#include <optional>
#include <string>
#include <vector>

namespace iv {
struct RuntimeGraphInputLanesPortsChangedRequest {
    std::vector<GraphInputPortDescriptor> ports;
};

struct RuntimeGraphInputLanesLiveInputSnapshotRequest {
    std::string logical_node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample fallback = Sample {0.0f};
};

struct RuntimeGraphInputLanesLiveInputSnapshot {
    std::string logical_node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample current_value = Sample {0.0f};
    bool has_concrete_override = false;
};

struct RuntimeGraphInputLanesLaneOutputs {
    LaneId lane;
    std::vector<LaneOutputConnection> outputs;
};

struct RuntimeGraphInputLanesSetSampleInputValueRequest {
    std::string node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample value = Sample {0.0f};
    GraphInputPortDescriptor graph_input_port;
};

struct RuntimeGraphInputLanesClearSampleInputValueOverrideRequest {
    std::string node_id;
    size_t member_ordinal = 0;
    size_t input_ordinal = 0;
    GraphInputPortDescriptor graph_input_port;
};

class RuntimeGraphInputLanesAckBuilder {
    std::optional<std::string> error_message;
    bool handled = false;

public:
    void succeed();
    void fail(std::string message);
    void build() const;
};

class RuntimeGraphInputLanesLiveInputSnapshotsBuilder {
    std::optional<std::vector<RuntimeGraphInputLanesLiveInputSnapshot>> result;

public:
    void succeed(std::vector<RuntimeGraphInputLanesLiveInputSnapshot> value);
    [[nodiscard]] std::vector<RuntimeGraphInputLanesLiveInputSnapshot> build() const;
};

class RuntimeGraphInputLanesLaneBindingsBuilder {
    std::optional<GraphInputLaneBindings> result;

public:
    void succeed(GraphInputLaneBindings value);
    [[nodiscard]] GraphInputLaneBindings build() const;
};

class RuntimeGraphInputLanesLaneOutputsBuilder {
    std::optional<std::vector<RuntimeGraphInputLanesLaneOutputs>> result;

public:
    void succeed(std::vector<RuntimeGraphInputLanesLaneOutputs> value);
    [[nodiscard]] std::vector<RuntimeGraphInputLanesLaneOutputs> build() const;
};

using RuntimeGraphInputLanesPortsChangedEvent =
    void (*)(RuntimeGraphInputLanesPortsChangedRequest const &, RuntimeGraphInputLanesAckBuilder &);
using RuntimeGraphInputLanesLiveInputSnapshotsRequestedEvent =
    void (*)(std::vector<RuntimeGraphInputLanesLiveInputSnapshotRequest> const &, RuntimeGraphInputLanesLiveInputSnapshotsBuilder &);
using RuntimeGraphInputLanesLaneBindingsEnsuredEvent =
    void (*)(RuntimeGraphInputLanesPortsChangedRequest const &, RuntimeGraphInputLanesAckBuilder &);
using RuntimeGraphInputLanesLaneBindingsRequestedEvent =
    void (*)(RuntimeGraphInputLanesPortsChangedRequest const &, RuntimeGraphInputLanesLaneBindingsBuilder &);
using RuntimeGraphInputLanesLaneOutputsRequestedEvent =
    void (*)(std::vector<LaneId> const &, RuntimeGraphInputLanesLaneOutputsBuilder &);
using RuntimeGraphInputLanesSetSampleInputValueRequestedEvent =
    void (*)(RuntimeGraphInputLanesSetSampleInputValueRequest const &, RuntimeGraphInputLanesAckBuilder &);
using RuntimeGraphInputLanesClearSampleInputValueOverrideRequestedEvent =
    void (*)(RuntimeGraphInputLanesClearSampleInputValueOverrideRequest const &, RuntimeGraphInputLanesAckBuilder &);
using RuntimeGraphInputLanesLaneViewUpdatedEvent =
    void (*)(LaneViewResult const &);

IV_DECLARE_LINKER_EVENT(
    RuntimeGraphInputLanesPortsChangedEvent,
    iv_runtime_graph_input_lanes_ports_changed_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeGraphInputLanesLiveInputSnapshotsRequestedEvent,
    iv_runtime_graph_input_lanes_live_input_snapshots_requested_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeGraphInputLanesLaneBindingsEnsuredEvent,
    iv_runtime_graph_input_lanes_lane_bindings_ensured_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeGraphInputLanesLaneBindingsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_bindings_requested_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeGraphInputLanesLaneOutputsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_outputs_requested_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeGraphInputLanesSetSampleInputValueRequestedEvent,
    iv_runtime_graph_input_lanes_set_sample_input_value_requested_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeGraphInputLanesClearSampleInputValueOverrideRequestedEvent,
    iv_runtime_graph_input_lanes_clear_sample_input_value_override_requested_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeGraphInputLanesLaneViewUpdatedEvent,
    iv_runtime_graph_input_lanes_lane_view_updated_event);
} // namespace iv
