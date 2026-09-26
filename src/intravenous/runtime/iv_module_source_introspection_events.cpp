#include <intravenous/runtime/iv_module_source_introspection_events.h>

#include <stdexcept>
#include <utility>

namespace iv {
void IvModuleInstancesSourceFileFilterBuilder::succeed(
    std::vector<IvModuleInstanceInfo> value)
{
    result = std::move(value);
}

bool IvModuleInstancesSourceFileFilterBuilder::has_response() const
{
    return result.has_value();
}

std::vector<IvModuleInstanceInfo> IvModuleInstancesSourceFileFilterBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error("IV module source file filter was not handled");
    }
    return *result;
}
} // namespace iv
