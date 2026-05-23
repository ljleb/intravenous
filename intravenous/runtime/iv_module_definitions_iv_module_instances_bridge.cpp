#include "runtime/iv_module_definitions_iv_module_instances_bridge.h"

#include "runtime/iv_module_definitions_events.h"
#include "runtime/iv_module_instances.h"

namespace iv {
namespace {
RuntimeIvModuleInstances *bound_instances = nullptr;

void handle_definitions_changed(RuntimeIvModuleDefinitionsChanged const &diff)
{
    if (bound_instances == nullptr) {
        return;
    }
    bound_instances->handle_iv_module_definitions_changed(diff);
}

IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeIvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event,
    handle_definitions_changed);
} // namespace

void bind_iv_module_definitions_iv_module_instances_bridge(
    RuntimeIvModuleInstances &instances)
{
    bound_instances = &instances;
}

void unbind_iv_module_definitions_iv_module_instances_bridge(
    RuntimeIvModuleInstances const &instances)
{
    if (bound_instances == &instances) {
        bound_instances = nullptr;
    }
}
} // namespace iv
