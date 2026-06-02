#pragma once

#include "linker_event.h"
#include "runtime/runtime_project_api_types.h"

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

class IvModuleSourceIntrospectionLiveInputSnapshotsBuilder {
    std::optional<std::vector<IvModuleSourceIntrospectionLiveInputSnapshot>> result;

public:
    void succeed(std::vector<IvModuleSourceIntrospectionLiveInputSnapshot> value);
    [[nodiscard]] std::vector<IvModuleSourceIntrospectionLiveInputSnapshot> build() const;
};

using IvModuleSourceIntrospectionLiveInputSnapshotsRequestedEvent =
    void (*)(std::vector<IvModuleSourceIntrospectionLiveInputSnapshotRequest> const &,
             IvModuleSourceIntrospectionLiveInputSnapshotsBuilder &);

IV_DECLARE_LINKER_EVENT(
    IvModuleSourceIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_iv_module_source_introspection_live_input_snapshots_requested_event);
} // namespace iv
