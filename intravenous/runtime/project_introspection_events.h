#pragma once

#include "linker_event.h"
#include "runtime/runtime_project_api_types.h"

#include <optional>
#include <string>
#include <vector>

namespace iv {
struct RuntimeProjectIntrospectionLiveInputSnapshotRequest {
    std::string logical_node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample fallback = Sample{0.0f};
};

struct RuntimeProjectIntrospectionLiveInputSnapshot {
    std::string logical_node_id;
    std::optional<size_t> member_ordinal;
    size_t input_ordinal = 0;
    Sample current_value = Sample{0.0f};
    bool has_concrete_override = false;
};

class RuntimeProjectIntrospectionLiveInputSnapshotsBuilder {
    std::optional<std::vector<RuntimeProjectIntrospectionLiveInputSnapshot>> result;

public:
    void succeed(std::vector<RuntimeProjectIntrospectionLiveInputSnapshot> value);
    [[nodiscard]] std::vector<RuntimeProjectIntrospectionLiveInputSnapshot> build() const;
};

using RuntimeProjectIntrospectionLiveInputSnapshotsRequestedEvent =
    void (*)(std::vector<RuntimeProjectIntrospectionLiveInputSnapshotRequest> const &,
             RuntimeProjectIntrospectionLiveInputSnapshotsBuilder &);

IV_DECLARE_LINKER_EVENT(
    RuntimeProjectIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_project_introspection_live_input_snapshots_requested_event);
} // namespace iv
