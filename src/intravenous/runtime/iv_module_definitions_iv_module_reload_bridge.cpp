#include <intravenous/runtime/iv_module_definitions_iv_module_reload_bridge.h>

#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/iv_module_reload_events.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_definitions_iv_module_reload_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_definitions_iv_module_reload_bridge,
    iv_runtime_iv_module_definitions_declarations_changed_event,
    &IvModuleReload::handle_definition_declarations_changed);
IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_definitions_iv_module_reload_bridge,
    iv_runtime_iv_module_reload_results_event,
    &IvModuleDefinitions::handle_reload_results);
} // namespace iv
