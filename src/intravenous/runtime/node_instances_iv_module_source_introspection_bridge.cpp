#include <intravenous/runtime/node_instances_iv_module_source_introspection_bridge.h>

#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>

namespace iv {
IV_DEFINE_BRIDGE(node_instances_iv_module_source_introspection_bridge)

// The instance-declaration updates of source introspection come from
// ProjectGraph once per root build
// (project_graph_iv_module_source_introspection_bridge).
IV_SUBSCRIBE_LINKER_EVENT(
    node_instances_iv_module_source_introspection_bridge,
    iv_runtime_iv_module_instances_source_file_filter_event,
    &IvModuleSourceIntrospection::handle_iv_module_instances_source_file_filter)
} // namespace iv
