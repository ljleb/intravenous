#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleSourceIntrospection;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_iv_module_source_introspection_bridge,
    SocketRpcServer,
    IvModuleSourceIntrospection);
} // namespace iv
