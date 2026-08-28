#include <intravenous/runtime/socket_rpc_iv_module_instances_bridge.h>

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_iv_module_instances_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_instances_bridge,
    iv_socket_rpc_get_iv_module_instances_event,
    &IvModuleInstances::handle_socket_rpc_get_iv_module_instances)
} // namespace iv
