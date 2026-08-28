#include <intravenous/runtime/socket_rpc_iv_module_source_introspection_bridge.h>

#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/socket_rpc_server.h>

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
    iv_socket_rpc_get_virtual_node_event,
    &IvModuleSourceIntrospection::handle_socket_rpc_get_virtual_node)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_source_introspection_bridge,
    iv_socket_rpc_get_virtual_nodes_event,
    &IvModuleSourceIntrospection::handle_socket_rpc_get_virtual_nodes)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_source_introspection_bridge,
    iv_socket_rpc_set_sample_input_value_event,
    &IvModuleSourceIntrospection::handle_socket_rpc_set_sample_input_value)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_source_introspection_bridge,
    iv_socket_rpc_set_sample_input_state_event,
    &IvModuleSourceIntrospection::handle_socket_rpc_set_sample_input_state)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_source_introspection_bridge,
    iv_socket_rpc_set_event_input_state_event,
    &IvModuleSourceIntrospection::handle_socket_rpc_set_event_input_state)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_source_introspection_bridge,
    iv_socket_rpc_set_sample_output_state_event,
    &IvModuleSourceIntrospection::handle_socket_rpc_set_sample_output_state)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_source_introspection_bridge,
    iv_socket_rpc_set_event_output_state_event,
    &IvModuleSourceIntrospection::handle_socket_rpc_set_event_output_state)
} // namespace iv
