#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleInstances;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_iv_module_instances_bridge,
    SocketRpcServer,
    IvModuleInstances);
} // namespace iv
