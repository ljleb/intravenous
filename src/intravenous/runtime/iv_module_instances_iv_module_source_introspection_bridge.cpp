#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>

#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_module_source_introspection.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_instances_iv_module_source_introspection_bridge)
IV_SUBSCRIBE_BRIDGE(
    iv_module_instances_iv_module_source_introspection_bridge,
    iv_runtime_iv_module_instances_list_changed_event,
    &IvModuleSourceIntrospection::handle_iv_module_instances_list_changed);
} // namespace iv
