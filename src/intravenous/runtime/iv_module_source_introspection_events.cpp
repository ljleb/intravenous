#include <intravenous/runtime/iv_module_source_introspection_events.h>

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

void IvModuleSourceIntrospectionAuthoredStateSnapshotBuilder::succeed(
    IvModuleSourceIntrospectionAuthoredStateSnapshot value)
{
    result = std::move(value);
}

IvModuleSourceIntrospectionAuthoredStateSnapshot
IvModuleSourceIntrospectionAuthoredStateSnapshotBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime project introspection authored state snapshot request was not handled");
    }
    return *result;
}

void IvModuleSourceIntrospectionPublicPortsSnapshotBuilder::succeed(
    IvModuleSourceIntrospectionPublicPortsSnapshot value)
{
    result = std::move(value);
}

IvModuleSourceIntrospectionPublicPortsSnapshot
IvModuleSourceIntrospectionPublicPortsSnapshotBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime project introspection public ports snapshot request was not handled");
    }
    return *result;
}

void IvModuleInstancesSourceFileFilterBuilder::succeed(
    std::vector<IvModuleInstanceInfo> value)
{
    result = std::move(value);
}

bool IvModuleInstancesSourceFileFilterBuilder::has_response() const
{
    return result.has_value();
}

std::vector<IvModuleInstanceInfo>
IvModuleInstancesSourceFileFilterBuilder::build() const
{
    return *result;
}

} // namespace iv
