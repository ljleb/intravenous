#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/runtime_project_api_types.h>

#include <optional>
#include <string>
#include <vector>

namespace iv {
struct IvModuleSourceIntrospectionLiveInputSnapshotRequest {
    std::string logical_node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample fallback = Sample{0.0f};
};

struct IvModuleSourceIntrospectionLiveInputSnapshot {
    std::string logical_node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample current_value = Sample{0.0f};
    bool has_concrete_override = false;
};

struct IvModuleSourceIntrospectionAuthoredStateSnapshot {
    std::vector<ProjectSetSampleInputValueRequest> sample_input_values {};
    std::vector<ProjectSetSampleInputStateRequest> sample_input_states {};
    std::vector<ProjectSetEventInputStateRequest> event_input_states {};
    std::vector<ProjectSetSampleOutputStateRequest> sample_output_states {};
    std::vector<ProjectSetEventOutputStateRequest> event_output_states {};
};

class IvModuleSourceIntrospectionLiveInputSnapshotsBuilder {
    std::optional<std::vector<IvModuleSourceIntrospectionLiveInputSnapshot>> result;

public:
    void succeed(std::vector<IvModuleSourceIntrospectionLiveInputSnapshot> value);
    [[nodiscard]] std::vector<IvModuleSourceIntrospectionLiveInputSnapshot> build() const;
};

class IvModuleSourceIntrospectionAuthoredStateSnapshotBuilder {
    std::optional<IvModuleSourceIntrospectionAuthoredStateSnapshot> result;

public:
    void succeed(IvModuleSourceIntrospectionAuthoredStateSnapshot value);
    [[nodiscard]] IvModuleSourceIntrospectionAuthoredStateSnapshot build() const;
};

using IvModuleSourceIntrospectionLiveInputSnapshotsRequestedEvent =
    void (*)(std::vector<IvModuleSourceIntrospectionLiveInputSnapshotRequest> const &,
             IvModuleSourceIntrospectionLiveInputSnapshotsBuilder &);
using IvModuleSourceIntrospectionAuthoredStateSnapshotRequestedEvent =
    void (*)(IvModuleSourceIntrospectionAuthoredStateSnapshotBuilder &);
using IvModuleSourceIntrospectionNodesUpdatedEvent =
    void (*)(ProjectLogicalNodesNotification const &);

IV_DECLARE_LINKER_EVENT(
    IvModuleSourceIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_iv_module_source_introspection_live_input_snapshots_requested_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleSourceIntrospectionAuthoredStateSnapshotRequestedEvent,
    iv_runtime_iv_module_source_introspection_authored_state_snapshot_requested_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleSourceIntrospectionNodesUpdatedEvent,
    iv_runtime_iv_module_source_introspection_nodes_updated_event);
} // namespace iv
