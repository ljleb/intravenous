#include <intravenous/app/bridges/socket_rpc_node_instances_bridge.h>

#include <intravenous/project/node_instances.h>
#include <intravenous/rpc/socket_rpc_server.h>

#include <intravenous/project/iv_module_instances_events.h>
namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_node_instances_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_node_instances_bridge,
    iv_socket_rpc_get_node_instances_event,
    &NodeInstances::handle_socket_rpc_get_node_instances)
} // namespace iv
