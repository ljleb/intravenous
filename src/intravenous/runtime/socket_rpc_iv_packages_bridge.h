#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvPackages;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_iv_packages_bridge,
    SocketRpcServer,
    IvPackages);
} // namespace iv
