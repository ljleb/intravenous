#include <intravenous/runtime/project_graph_iv_module_source_introspection_bridge.h>

#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/project_graph_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_graph_iv_module_source_introspection_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    project_graph_iv_module_source_introspection_bridge,
    iv_runtime_project_graph_source_introspection_requested_event,
    &IvModuleSourceIntrospection::handle_project_graph_source_introspection)
} // namespace iv
