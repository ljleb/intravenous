#include <intravenous/runtime/project_persistence_project_graph_bridge.h>

#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_project_graph_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_create_iv_module_instance_requested_event,
    &ProjectGraph::handle_project_create_iv_module_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_delete_iv_module_instance_requested_event,
    &ProjectGraph::handle_project_delete_iv_module_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_update_iv_module_instances_requested_event,
    &ProjectGraph::handle_project_update_iv_module_instances)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_upsert_graph_connection_requested_event,
    &ProjectGraph::handle_project_upsert_graph_connection)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_delete_graph_connection_requested_event,
    &ProjectGraph::handle_project_delete_graph_connection)
} // namespace iv
