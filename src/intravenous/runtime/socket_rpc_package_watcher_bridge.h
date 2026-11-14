#pragma once

#include <intravenous/bridge.h>

namespace iv {
class PackageWatcher;
class SocketRpcServer;

// The one report of each package refresh, sent by PackageWatcher after the
// refresh's propagation has returned.
IV_DECLARE_BRIDGE(
    socket_rpc_package_watcher_bridge,
    SocketRpcServer,
    PackageWatcher);
} // namespace iv
