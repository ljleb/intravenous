#include <intravenous/runtime/iv_module_reload_iv_module_definitions_bridge.h>

#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_reload_events.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_reload_iv_module_definitions_bridge)
IV_SUBSCRIBE_BRIDGE(
    iv_module_reload_iv_module_definitions_bridge,
    iv_runtime_iv_module_reload_results_event,
    &IvModuleDefinitions::handle_reload_results);
} // namespace iv
