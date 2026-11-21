#pragma once
#include <intravenous/bridge.h>
namespace iv {
class PackageDefinitions;
class SocketRpcServer;
IV_DECLARE_BRIDGE(
    socket_rpc_package_definitions_bridge,
    SocketRpcServer,
    PackageDefinitions);
}
