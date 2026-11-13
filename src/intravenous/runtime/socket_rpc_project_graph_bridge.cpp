#include <intravenous/runtime/socket_rpc_project_graph_bridge.h>

#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_project_graph_bridge)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_apply_project_edit_event, &ProjectGraph::handle_socket_rpc_apply_project_edit)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_undo_project_edit_event, &ProjectGraph::handle_socket_rpc_undo_project_edit)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_redo_project_edit_event, &ProjectGraph::handle_socket_rpc_redo_project_edit)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_get_graph_scope_event,
    &ProjectGraph::handle_socket_rpc_get_graph_scope)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_create_node_instance_event,
    &ProjectGraph::handle_socket_rpc_create_node_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_delete_node_instance_event,
    &ProjectGraph::handle_socket_rpc_delete_node_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_update_node_instances_event,
    &ProjectGraph::handle_socket_rpc_update_node_instances)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_upsert_graph_connection_event,
    &ProjectGraph::handle_socket_rpc_upsert_graph_connection)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_delete_graph_connection_event,
    &ProjectGraph::handle_socket_rpc_delete_graph_connection)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_upsert_manual_control_event,
    &ProjectGraph::handle_socket_rpc_upsert_manual_control)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_delete_manual_control_event,
    &ProjectGraph::handle_socket_rpc_delete_manual_control)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_set_manual_control_hover_event,
    &ProjectGraph::handle_socket_rpc_set_manual_control_hover)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_begin_manual_control_interaction_event,
    &ProjectGraph::handle_socket_rpc_begin_manual_control_interaction)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_update_manual_control_interaction_event,
    &ProjectGraph::handle_socket_rpc_update_manual_control_interaction)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_end_manual_control_interaction_event,
    &ProjectGraph::handle_socket_rpc_end_manual_control_interaction)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_client_disconnected_event,
    &ProjectGraph::handle_socket_rpc_client_disconnected)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_get_node_event_protocol_event,
    &ProjectGraph::handle_socket_rpc_get_node_event_protocol)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_dispatch_node_message_event,
    &ProjectGraph::handle_socket_rpc_dispatch_node_message)
} // namespace iv
