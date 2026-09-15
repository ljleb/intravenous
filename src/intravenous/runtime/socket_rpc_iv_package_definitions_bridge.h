#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvPackageDefinitions;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_iv_package_definitions_bridge,
    SocketRpcServer,
    IvPackageDefinitions);
} // namespace iv
