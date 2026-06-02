#include "runtime/iv_module_instances_iv_module_definitions_bridge.h"

#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_instances_events.h"

namespace iv {
namespace {
IvModuleDefinitions *bound_definitions = nullptr;

void handle_required_definitions_changed(
    IvModuleRequiredDefinitionsChanged const &diff)
{
    if (bound_definitions == nullptr) {
        return;
    }
    bound_definitions->handle_required_definitions_changed(diff);
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleRequiredDefinitionsChangedEvent,
    iv_runtime_iv_module_required_definitions_changed_event,
    handle_required_definitions_changed);
} // namespace

void bind_iv_module_instances_iv_module_definitions_bridge(
    IvModuleDefinitions &definitions)
{
    bound_definitions = &definitions;
}

void unbind_iv_module_instances_iv_module_definitions_bridge(
    IvModuleDefinitions const &definitions)
{
    if (bound_definitions == &definitions) {
        bound_definitions = nullptr;
    }
}
} // namespace iv
