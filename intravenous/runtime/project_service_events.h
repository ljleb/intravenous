#pragma once

#include "linker_event.h"
#include "runtime/graph_input_lane_controller.h"
#include "runtime/runtime_project_api_types.h"

#include <optional>
#include <string>
#include <vector>

namespace iv {
    struct RuntimeProjectLiveInputSnapshotRequest {
        std::string logical_node_id;
        std::optional<size_t> member_ordinal;
        size_t input_ordinal = 0;
        Sample fallback = Sample {0.0f};
    };

    struct RuntimeProjectLiveInputSnapshot {
        std::string logical_node_id;
        std::optional<size_t> member_ordinal;
        size_t input_ordinal = 0;
        Sample current_value = Sample {0.0f};
        bool has_concrete_override = false;
    };

    class RuntimeProjectAckBuilder {
        std::optional<std::string> error_message;
        bool handled = false;

    public:
        void succeed();
        void fail(std::string message);
        void build() const;
    };

    class RuntimeProjectLiveInputSnapshotsBuilder {
        std::optional<std::vector<RuntimeProjectLiveInputSnapshot>> result;

    public:
        void succeed(std::vector<RuntimeProjectLiveInputSnapshot> value);
        [[nodiscard]] std::vector<RuntimeProjectLiveInputSnapshot> build() const;
    };

    class RuntimeProjectGraphInputLaneBindingsBuilder {
        std::optional<GraphInputLaneBindings> result;

    public:
        void succeed(GraphInputLaneBindings value);
        [[nodiscard]] GraphInputLaneBindings build() const;
    };

    struct RuntimeProjectLaneOutputs {
        LaneId lane;
        std::vector<LaneOutputConnection> outputs;
    };

    class RuntimeProjectLaneOutputsBuilder {
        std::optional<std::vector<RuntimeProjectLaneOutputs>> result;

    public:
        void succeed(std::vector<RuntimeProjectLaneOutputs> value);
        [[nodiscard]] std::vector<RuntimeProjectLaneOutputs> build() const;
    };

    struct RuntimeProjectGraphInputLaneBindingsRequest {
        std::vector<GraphInputPortDescriptor> ports;
    };

    struct RuntimeProjectLaneOutputsRequest {
        std::vector<LaneId> lanes;
    };

    struct RuntimeProjectSetSampleInputValueRequest {
        std::string node_id;
        std::optional<size_t> member_ordinal;
        size_t input_ordinal = 0;
        Sample value = Sample {0.0f};
        GraphInputPortDescriptor graph_input_port;
    };

    struct RuntimeProjectClearSampleInputValueOverrideRequest {
        std::string node_id;
        size_t member_ordinal = 0;
        size_t input_ordinal = 0;
        GraphInputPortDescriptor graph_input_port;
    };

    using RuntimeProjectNotificationEvent =
        void (*)(RuntimeProjectNotification const &);
    using RuntimeProjectLiveInputSnapshotsRequestedEvent =
        void (*)(std::vector<RuntimeProjectLiveInputSnapshotRequest> const &, RuntimeProjectLiveInputSnapshotsBuilder &);
    using RuntimeProjectGraphInputLaneBindingsEnsuredEvent =
        void (*)(RuntimeProjectGraphInputLaneBindingsRequest const &, RuntimeProjectAckBuilder &);
    using RuntimeProjectGraphInputLaneBindingsRequestedEvent =
        void (*)(RuntimeProjectGraphInputLaneBindingsRequest const &, RuntimeProjectGraphInputLaneBindingsBuilder &);
    using RuntimeProjectLaneOutputsRequestedEvent =
        void (*)(RuntimeProjectLaneOutputsRequest const &, RuntimeProjectLaneOutputsBuilder &);
    using RuntimeProjectSetSampleInputValueRequestedEvent =
        void (*)(RuntimeProjectSetSampleInputValueRequest const &, RuntimeProjectAckBuilder &);
    using RuntimeProjectClearSampleInputValueOverrideRequestedEvent =
        void (*)(RuntimeProjectClearSampleInputValueOverrideRequest const &, RuntimeProjectAckBuilder &);

    IV_DECLARE_LINKER_EVENT(
        RuntimeProjectNotificationEvent,
        iv_runtime_project_notification_event);
    IV_DECLARE_LINKER_EVENT(
        RuntimeProjectLiveInputSnapshotsRequestedEvent,
        iv_runtime_project_live_input_snapshots_requested_event);
    IV_DECLARE_LINKER_EVENT(
        RuntimeProjectGraphInputLaneBindingsEnsuredEvent,
        iv_runtime_project_graph_input_lane_bindings_ensured_event);
    IV_DECLARE_LINKER_EVENT(
        RuntimeProjectGraphInputLaneBindingsRequestedEvent,
        iv_runtime_project_graph_input_lane_bindings_requested_event);
    IV_DECLARE_LINKER_EVENT(
        RuntimeProjectLaneOutputsRequestedEvent,
        iv_runtime_project_lane_outputs_requested_event);
    IV_DECLARE_LINKER_EVENT(
        RuntimeProjectSetSampleInputValueRequestedEvent,
        iv_runtime_project_set_sample_input_value_requested_event);
    IV_DECLARE_LINKER_EVENT(
        RuntimeProjectClearSampleInputValueOverrideRequestedEvent,
        iv_runtime_project_clear_sample_input_value_override_requested_event);
} // namespace iv
