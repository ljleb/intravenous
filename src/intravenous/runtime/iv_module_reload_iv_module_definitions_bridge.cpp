#include "runtime/iv_module_reload_iv_module_definitions_bridge.h"

#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_reload_events.h"

namespace iv {
namespace {
IvModuleDefinitions *bound_definitions = nullptr;

void handle_reload_results(IvModuleReloadResults const &results)
{
    if (bound_definitions == nullptr) {
        return;
    }
    bound_definitions->handle_reload_results(results);
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleReloadResultsEvent,
    iv_runtime_iv_module_reload_results_event,
    handle_reload_results);
} // namespace

void bind_iv_module_reload_iv_module_definitions_bridge(
    IvModuleDefinitions &definitions)
{
    bound_definitions = &definitions;
}

void unbind_iv_module_reload_iv_module_definitions_bridge(
    IvModuleDefinitions const &definitions)
{
    if (bound_definitions == &definitions) {
        bound_definitions = nullptr;
    }
}
} // namespace iv
