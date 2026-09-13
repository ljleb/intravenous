#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleDefinitions;
class SocketRpcServer;

// The package browser refreshes from an authoritative registry snapshot. Keep
// that projection separate from diagnostic notifications so neither bridge
// subscribes to two unrelated definitions event families.
IV_DECLARE_BRIDGE(
    iv_module_definitions_socket_rpc_packages_bridge,
    IvModuleDefinitions,
    SocketRpcServer);
} // namespace iv
