#include <intravenous/runtime/socket_rpc_iv_packages_bridge.h>

#include <intravenous/runtime/iv_packages.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_iv_packages_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_packages_bridge,
    iv_socket_rpc_get_iv_packages_event,
    &IvPackages::handle_socket_rpc_get_iv_packages)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_packages_bridge,
    iv_socket_rpc_create_iv_module_source_event,
    &IvPackages::handle_socket_rpc_create_iv_module_source)
} // namespace iv
