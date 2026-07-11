#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>

#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_instances.h>

namespace iv {
namespace {
IvModuleInstances *bound_instances = nullptr;

void handle_definitions_changed(IvModuleDefinitionsChanged const &diff)
{
    if (bound_instances == nullptr) {
        return;
    }
    bound_instances->handle_iv_module_definitions_changed(diff);
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event,
    handle_definitions_changed);
} // namespace

void bind_iv_module_definitions_iv_module_instances_bridge(
    IvModuleInstances &instances)
{
    bound_instances = &instances;
}

void unbind_iv_module_definitions_iv_module_instances_bridge(
    IvModuleInstances const &instances)
{
    if (bound_instances == &instances) {
        bound_instances = nullptr;
    }
}
} // namespace iv
