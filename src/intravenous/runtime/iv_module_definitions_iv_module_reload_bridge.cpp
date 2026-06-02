#include "runtime/iv_module_definitions_iv_module_reload_bridge.h"

#include "runtime/iv_module_definitions_events.h"
#include "runtime/iv_module_reload.h"

namespace iv {
namespace {
IvModuleReload *bound_reload = nullptr;

void handle_definition_declarations_changed(
    IvModuleDefinitionDeclarationsChanged const &diff)
{
    if (bound_reload == nullptr) {
        return;
    }
    bound_reload->handle_definition_declarations_changed(diff);
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleDefinitionDeclarationsChangedEvent,
    iv_runtime_iv_module_definitions_declarations_changed_event,
    handle_definition_declarations_changed);
} // namespace

void bind_iv_module_definitions_iv_module_reload_bridge(
    IvModuleReload &reload)
{
    bound_reload = &reload;
}

void unbind_iv_module_definitions_iv_module_reload_bridge(
    IvModuleReload const &reload)
{
    if (bound_reload == &reload) {
        bound_reload = nullptr;
    }
}
} // namespace iv
