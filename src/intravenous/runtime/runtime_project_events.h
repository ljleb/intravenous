#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/graph_input_lane_controller.h>
#include <intravenous/runtime/runtime_project_api_types.h>

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

    struct ProjectLiveInputSnapshot {
        std::string logical_node_id;
        std::optional<size_t> member_ordinal;
        size_t input_ordinal = 0;
        Sample current_value = Sample {0.0f};
        bool has_concrete_override = false;
    };

    class ProjectAckBuilder {
        std::optional<std::string> error_message;
        bool handled = false;

    public:
        void succeed();
        void fail(std::string message);
        void build() const;
    };

    class ProjectLiveInputSnapshotsBuilder {
        std::optional<std::vector<ProjectLiveInputSnapshot>> result;

    public:
        void succeed(std::vector<ProjectLiveInputSnapshot> value);
        [[nodiscard]] std::vector<ProjectLiveInputSnapshot> build() const;
    };

    class ProjectGraphInputLaneBindingsBuilder {
        std::optional<GraphInputLaneBindings> result;

    public:
        void succeed(GraphInputLaneBindings value);
        [[nodiscard]] GraphInputLaneBindings build() const;
    };

    struct ProjectLaneOutputs {
        LaneId lane;
        std::vector<LaneOutputConnection> outputs;
    };

    class ProjectLaneOutputsBuilder {
        std::optional<std::vector<ProjectLaneOutputs>> result;

    public:
        void succeed(std::vector<ProjectLaneOutputs> value);
        [[nodiscard]] std::vector<ProjectLaneOutputs> build() const;
    };

    struct ProjectGraphInputLaneBindingsRequest {
        std::vector<GraphInputPortDescriptor> ports;
    };

    struct ProjectLaneOutputsRequest {
        std::vector<LaneId> lanes;
    };

    struct ProjectSetSampleInputValueRequest {
        std::string node_id;
        std::optional<size_t> member_ordinal;
        size_t input_ordinal = 0;
        Sample value = Sample {0.0f};
    };

    struct ProjectClearSampleInputValueOverrideRequest {
        std::string node_id;
        size_t member_ordinal = 0;
        size_t input_ordinal = 0;
    };

    using ProjectNotificationEvent =
        void (*)(ProjectNotification const &);
    using ProjectLiveInputSnapshotsRequestedEvent =
        void (*)(std::vector<RuntimeProjectLiveInputSnapshotRequest> const &, ProjectLiveInputSnapshotsBuilder &);
    using ProjectGraphInputLaneBindingsEnsuredEvent =
        void (*)(ProjectGraphInputLaneBindingsRequest const &, ProjectAckBuilder &);
    using ProjectGraphInputLaneBindingsRequestedEvent =
        void (*)(ProjectGraphInputLaneBindingsRequest const &, ProjectGraphInputLaneBindingsBuilder &);
    using ProjectLaneOutputsRequestedEvent =
        void (*)(ProjectLaneOutputsRequest const &, ProjectLaneOutputsBuilder &);
    using ProjectSetSampleInputValueRequestedEvent =
        void (*)(ProjectSetSampleInputValueRequest const &, ProjectAckBuilder &);
    using ProjectClearSampleInputValueOverrideRequestedEvent =
        void (*)(ProjectClearSampleInputValueOverrideRequest const &, ProjectAckBuilder &);

    IV_DECLARE_LINKER_EVENT(
        ProjectNotificationEvent,
        iv_runtime_project_notification_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectLiveInputSnapshotsRequestedEvent,
        iv_runtime_project_live_input_snapshots_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectGraphInputLaneBindingsEnsuredEvent,
        iv_runtime_project_graph_input_lane_bindings_ensured_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectGraphInputLaneBindingsRequestedEvent,
        iv_runtime_project_graph_input_lane_bindings_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectLaneOutputsRequestedEvent,
        iv_runtime_project_lane_outputs_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectSetSampleInputValueRequestedEvent,
        iv_runtime_project_set_sample_input_value_requested_event);
    IV_DECLARE_LINKER_EVENT(
        ProjectClearSampleInputValueOverrideRequestedEvent,
        iv_runtime_project_clear_sample_input_value_override_requested_event);
} // namespace iv
