#include <intravenous/runtime/socket_rpc_iv_module_sources_bridge.h>

#include <intravenous/runtime/iv_module_sources.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_iv_module_sources_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_sources_bridge,
    iv_socket_rpc_get_iv_module_sources_event,
    &IvModuleSources::handle_socket_rpc_get_iv_module_sources)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_iv_module_sources_bridge,
    iv_socket_rpc_create_iv_module_source_event,
    &IvModuleSources::handle_socket_rpc_create_iv_module_source)
} // namespace iv
