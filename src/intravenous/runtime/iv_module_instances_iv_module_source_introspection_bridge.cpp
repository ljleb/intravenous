#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>

#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_module_source_introspection.h>

namespace iv {
namespace {
IvModuleSourceIntrospection *bound_introspection = nullptr;

void handle_instances_list_changed(std::vector<IvModuleInstanceInfo> const &instances)
{
    if (bound_introspection == nullptr) {
        return;
    }
    bound_introspection->handle_iv_module_instances_list_changed(instances);
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleInstancesListChangedEvent,
    iv_runtime_iv_module_instances_list_changed_event,
    handle_instances_list_changed);
} // namespace

void bind_iv_module_instances_iv_module_source_introspection_bridge(
    IvModuleSourceIntrospection &introspection)
{
    bound_introspection = &introspection;
}

void unbind_iv_module_instances_iv_module_source_introspection_bridge(
    IvModuleSourceIntrospection const &introspection)
{
    if (bound_introspection == &introspection) {
        bound_introspection = nullptr;
    }
}
} // namespace iv
