#include <intravenous/runtime/node_definitions_iv_module_source_introspection_bridge.h>

#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/iv_module_source_introspection.h>

namespace iv {
IV_DEFINE_BRIDGE(node_definitions_iv_module_source_introspection_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_iv_module_source_introspection_bridge,
    iv_runtime_iv_package_definitions_changed_event,
    &IvModuleSourceIntrospection::handle_iv_package_definitions_changed)
} // namespace iv
