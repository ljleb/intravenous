#include <intravenous/runtime/iv_module_definitions_iv_packages_bridge.h>

#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_packages.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_definitions_iv_packages_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_definitions_iv_packages_bridge,
    iv_runtime_iv_module_definitions_changed_event,
    &IvPackages::handle_iv_module_definitions_changed);
IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_definitions_iv_packages_bridge,
    iv_runtime_iv_node_type_definitions_changed_event,
    &IvPackages::handle_iv_node_type_definitions_changed);
} // namespace iv
