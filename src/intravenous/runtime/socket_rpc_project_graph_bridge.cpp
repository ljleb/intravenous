#include <intravenous/runtime/socket_rpc_project_graph_bridge.h>

#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_project_graph_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_create_iv_module_instance_event,
    &ProjectGraph::handle_socket_rpc_create_iv_module_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_delete_iv_module_instance_event,
    &ProjectGraph::handle_socket_rpc_delete_iv_module_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_graph_bridge,
    iv_socket_rpc_update_iv_module_instances_event,
    &ProjectGraph::handle_socket_rpc_update_iv_module_instances)
} // namespace iv
