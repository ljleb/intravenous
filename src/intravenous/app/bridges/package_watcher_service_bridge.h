#pragma once
#include <intravenous/bridge.h>
namespace iv {
class PackageWatcherService;
class PackageWatcher;
IV_DECLARE_BRIDGE(
    package_watcher_service_bridge,
    PackageWatcherService,
    PackageWatcher);
}
