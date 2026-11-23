#include <intravenous/app/bridges/socket_rpc_iv_module_source_introspection_bridge.h>

#include <intravenous/packages/iv_module_source_introspection.h>
#include <intravenous/rpc/socket_rpc_server.h>

#include <intravenous/packages/iv_module_source_introspection_events.h>
namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_iv_module_source_introspection_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_source_introspection_bridge,
    iv_socket_rpc_graph_query_by_spans_event,
    &IvModuleSourceIntrospection::handle_socket_rpc_graph_query_by_spans)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_source_introspection_bridge,
    iv_socket_rpc_graph_query_active_regions_event,
    &IvModuleSourceIntrospection::handle_socket_rpc_graph_query_active_regions)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_source_introspection_bridge,
    iv_socket_rpc_get_symbolic_node_event,
    &IvModuleSourceIntrospection::handle_socket_rpc_get_symbolic_node)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_source_introspection_bridge,
    iv_socket_rpc_get_symbolic_nodes_event,
    &IvModuleSourceIntrospection::handle_socket_rpc_get_symbolic_nodes)
} // namespace iv
