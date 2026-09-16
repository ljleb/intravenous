#include <intravenous/runtime/node_definitions_iv_package_definitions_bridge.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/iv_package_definitions.h>
namespace iv {
IV_DEFINE_BRIDGE(node_definitions_iv_package_definitions_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_iv_package_definitions_bridge,
    iv_runtime_iv_package_declarations_changed_event,
    &IvPackageDefinitions::handle_package_declarations_changed)
IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_iv_package_definitions_bridge,
    iv_runtime_iv_package_definitions_changed_event,
    &IvPackageDefinitions::handle_package_definitions_changed)
}
