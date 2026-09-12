#include <intravenous/runtime/iv_module_definitions_socket_rpc_packages_bridge.h>

#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_definitions_socket_rpc_packages_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_definitions_socket_rpc_packages_bridge,
    iv_runtime_iv_package_catalog_changed_event,
    &SocketRpcServer::handle_iv_package_catalog_changed)
} // namespace iv
