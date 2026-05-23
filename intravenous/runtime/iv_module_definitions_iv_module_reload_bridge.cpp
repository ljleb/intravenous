#include "runtime/iv_module_definitions_iv_module_reload_bridge.h"

#include "runtime/iv_module_definitions_events.h"
#include "runtime/iv_module_reload.h"

namespace iv {
namespace {
RuntimeIvModuleReload *bound_reload = nullptr;

void handle_definition_declarations_changed(
    RuntimeIvModuleDefinitionDeclarationsChanged const &diff)
{
    if (bound_reload == nullptr) {
        return;
    }
    bound_reload->handle_definition_declarations_changed(diff);
}

IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeIvModuleDefinitionDeclarationsChangedEvent,
    iv_runtime_iv_module_definitions_declarations_changed_event,
    handle_definition_declarations_changed);
} // namespace

void bind_iv_module_definitions_iv_module_reload_bridge(
    RuntimeIvModuleReload &reload)
{
    bound_reload = &reload;
}

void unbind_iv_module_definitions_iv_module_reload_bridge(
    RuntimeIvModuleReload const &reload)
{
    if (bound_reload == &reload) {
        bound_reload = nullptr;
    }
}
} // namespace iv
