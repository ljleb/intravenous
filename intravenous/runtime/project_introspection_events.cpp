#include "runtime/project_introspection_events.h"

#include <stdexcept>

namespace iv {
void ProjectIntrospectionLiveInputSnapshotsBuilder::succeed(
    std::vector<ProjectIntrospectionLiveInputSnapshot> value)
{
    result = std::move(value);
}

std::vector<ProjectIntrospectionLiveInputSnapshot>
ProjectIntrospectionLiveInputSnapshotsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime project introspection live input snapshots request was not handled");
    }
    return *result;
}

IV_DEFINE_LINKER_EVENT(
    ProjectIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_project_introspection_live_input_snapshots_requested_event);
} // namespace iv
