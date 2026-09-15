#include <intravenous/runtime/socket_rpc_iv_package_definitions_bridge.h>

#include <intravenous/runtime/iv_package_definitions.h>
#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_iv_package_definitions_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_package_definitions_bridge,
    iv_socket_rpc_get_iv_package_definitions_event,
    &IvPackageDefinitions::handle_socket_rpc_get_iv_package_definitions)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_package_definitions_bridge,
    iv_socket_rpc_create_iv_package_event,
    &IvPackageDefinitions::handle_socket_rpc_create_iv_package)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_package_definitions_bridge,
    iv_runtime_iv_package_catalog_changed_event,
    &SocketRpcServer::handle_iv_package_catalog_changed)
} // namespace iv
