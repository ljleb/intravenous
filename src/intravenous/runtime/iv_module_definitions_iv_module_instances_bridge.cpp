#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>

#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_instances.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_definitions_iv_module_instances_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_definitions_iv_module_instances_bridge,
    iv_runtime_iv_module_definitions_changed_event,
    &IvModuleInstances::handle_iv_module_definitions_changed);
} // namespace iv
