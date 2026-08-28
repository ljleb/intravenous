#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleSources;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_iv_module_sources_bridge,
    SocketRpcServer,
    IvModuleSources);
} // namespace iv
