#include "runtime/iv_module_definitions_project_introspection_bridge.h"

#include "runtime/iv_module_definitions_events.h"
#include "runtime/project_introspection.h"

namespace iv {
namespace {
ProjectIntrospection *bound_introspection = nullptr;

void handle_definitions_changed(IvModuleDefinitionsChanged const &diff)
{
    if (bound_introspection == nullptr) {
        return;
    }
    bound_introspection->handle_iv_module_definitions_changed(diff);
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleDefinitionsChangedEvent,
    iv_runtime_iv_module_definitions_changed_event,
    handle_definitions_changed);
} // namespace

void bind_iv_module_definitions_project_introspection_bridge(
    ProjectIntrospection &introspection)
{
    bound_introspection = &introspection;
}

void unbind_iv_module_definitions_project_introspection_bridge(
    ProjectIntrospection const &introspection)
{
    if (bound_introspection == &introspection) {
        bound_introspection = nullptr;
    }
}
} // namespace iv
