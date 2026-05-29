#include "runtime/iv_module_source_introspection_events.h"

#include <stdexcept>

namespace iv {
void IvModuleSourceIntrospectionLiveInputSnapshotsBuilder::succeed(
    std::vector<IvModuleSourceIntrospectionLiveInputSnapshot> value)
{
    result = std::move(value);
}

std::vector<IvModuleSourceIntrospectionLiveInputSnapshot>
IvModuleSourceIntrospectionLiveInputSnapshotsBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime project introspection live input snapshots request was not handled");
    }
    return *result;
}

IV_DEFINE_LINKER_EVENT(
    IvModuleSourceIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_iv_module_source_introspection_live_input_snapshots_requested_event);
} // namespace iv
