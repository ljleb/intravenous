#include <intravenous/runtime/project_persistence_project_graph_bridge.h>

#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_project_graph_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_create_node_instance_requested_event,
    &ProjectGraph::handle_project_create_node_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_delete_node_instance_requested_event,
    &ProjectGraph::handle_project_delete_node_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_update_node_instances_requested_event,
    &ProjectGraph::handle_project_update_node_instances)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_upsert_graph_connection_requested_event,
    &ProjectGraph::handle_project_upsert_graph_connection)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_delete_graph_connection_requested_event,
    &ProjectGraph::handle_project_delete_graph_connection)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_replace_graph_requested_event,
    &ProjectGraph::handle_project_replace_graph)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_upsert_manual_control_requested_event,
    &ProjectGraph::handle_project_upsert_manual_control)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_delete_manual_control_requested_event,
    &ProjectGraph::handle_project_delete_manual_control)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_set_manual_control_hover_requested_event,
    &ProjectGraph::handle_project_set_manual_control_hover)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_begin_manual_control_interaction_requested_event,
    &ProjectGraph::handle_project_begin_manual_control_interaction)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_update_manual_control_interaction_requested_event,
    &ProjectGraph::handle_project_update_manual_control_interaction)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_end_manual_control_interaction_requested_event,
    &ProjectGraph::handle_project_end_manual_control_interaction)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_graph_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &ProjectGraph::handle_project_persistence_collect_state)
} // namespace iv
