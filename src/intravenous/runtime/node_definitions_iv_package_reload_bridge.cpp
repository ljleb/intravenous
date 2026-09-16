#include <intravenous/runtime/node_definitions_iv_package_reload_bridge.h>

#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/iv_package_reload.h>
#include <intravenous/runtime/iv_package_reload_events.h>

namespace iv {
IV_DEFINE_BRIDGE(node_definitions_iv_package_reload_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_iv_package_reload_bridge,
    iv_runtime_iv_package_declarations_changed_event,
    &IvPackageReload::handle_package_declarations_changed);
IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_iv_package_reload_bridge,
    iv_runtime_iv_package_reload_results_event,
    &NodeDefinitions::handle_reload_results);
} // namespace iv
