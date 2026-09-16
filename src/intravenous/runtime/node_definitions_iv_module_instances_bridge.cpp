#include <intravenous/runtime/node_definitions_iv_module_instances_bridge.h>

#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_events.h>

namespace iv {
IV_DEFINE_BRIDGE(node_definitions_iv_module_instances_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_iv_module_instances_bridge,
    iv_runtime_iv_package_definitions_changed_event,
    &IvModuleInstances::handle_iv_package_definitions_changed);
IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_iv_module_instances_bridge,
    iv_runtime_iv_module_required_definitions_changed_event,
    &NodeDefinitions::handle_required_definitions_changed);
} // namespace iv
