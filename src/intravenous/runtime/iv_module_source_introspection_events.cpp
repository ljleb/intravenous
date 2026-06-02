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

} // namespace iv
