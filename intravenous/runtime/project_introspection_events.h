#pragma once

#include "linker_event.h"
#include "runtime/runtime_project_api_types.h"

#include <optional>
#include <string>
#include <vector>

namespace iv {
struct ProjectIntrospectionLiveInputSnapshotRequest {
    std::string logical_node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample fallback = Sample{0.0f};
};

struct ProjectIntrospectionLiveInputSnapshot {
    std::string logical_node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample current_value = Sample{0.0f};
    bool has_concrete_override = false;
};

class ProjectIntrospectionLiveInputSnapshotsBuilder {
    std::optional<std::vector<ProjectIntrospectionLiveInputSnapshot>> result;

public:
    void succeed(std::vector<ProjectIntrospectionLiveInputSnapshot> value);
    [[nodiscard]] std::vector<ProjectIntrospectionLiveInputSnapshot> build() const;
};

using ProjectIntrospectionLiveInputSnapshotsRequestedEvent =
    void (*)(std::vector<ProjectIntrospectionLiveInputSnapshotRequest> const &,
             ProjectIntrospectionLiveInputSnapshotsBuilder &);

IV_DECLARE_LINKER_EVENT(
    ProjectIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_project_introspection_live_input_snapshots_requested_event);
} // namespace iv
