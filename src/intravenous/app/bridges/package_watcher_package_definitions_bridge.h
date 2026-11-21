#pragma once
#include <intravenous/bridge.h>
namespace iv {
class PackageWatcher;
class PackageDefinitions;
IV_DECLARE_BRIDGE(
    package_watcher_package_definitions_bridge,
    PackageWatcher,
    PackageDefinitions);
}
