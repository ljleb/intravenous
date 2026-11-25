#include <intravenous/app/bridges/socket_rpc_project_graph_bridge.h>

#include <intravenous/project/project_graph.h>
#include <intravenous/project/project_graph_events.h>
#include <intravenous/project/runtime_project_events.h>
#include <intravenous/rpc/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_project_graph_bridge)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_runtime_project_graph_execution_published_event, &SocketRpcServer::send_project_graph_publication)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_runtime_control_compile_status_event, &SocketRpcServer::handle_control_compile_status)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_set_audio_devices_event, &ProjectGraph::handle_socket_rpc_set_audio_devices)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_set_project_toolchain_event, &ProjectGraph::handle_socket_rpc_set_project_toolchain)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_apply_project_edit_event, &ProjectGraph::handle_socket_rpc_apply_project_edit)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_reset_graph_state_event, &ProjectGraph::handle_socket_rpc_reset_graph_state)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_undo_project_edit_event, &ProjectGraph::handle_socket_rpc_undo_project_edit)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_redo_project_edit_event, &ProjectGraph::handle_socket_rpc_redo_project_edit)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_get_graph_scope_event,
    &ProjectGraph::handle_socket_rpc_get_graph_scope)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_get_project_lane_views_event, &ProjectGraph::handle_socket_rpc_get_project_lane_views)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_get_node_definitions_event, &ProjectGraph::handle_socket_rpc_get_node_definitions)
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
    iv_socket_rpc_toggle_manual_input_event,
    &ProjectGraph::handle_socket_rpc_toggle_manual_input)
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
    iv_socket_rpc_live_manual_control_update_event,
    &ProjectGraph::handle_live_manual_control_update)
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
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_project_graph_bridge,
    iv_socket_rpc_node_message_gesture_event, &ProjectGraph::handle_socket_rpc_node_message_gesture)
} // namespace iv
