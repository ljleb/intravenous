#include <intravenous/runtime/iv_module_instances_iv_module_definitions_bridge.h>

#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_instances_events.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_instances_iv_module_definitions_bridge)
IV_SUBSCRIBE_BRIDGE(
    iv_module_instances_iv_module_definitions_bridge,
    iv_runtime_iv_module_required_definitions_changed_event,
    &IvModuleDefinitions::handle_required_definitions_changed);
} // namespace iv
