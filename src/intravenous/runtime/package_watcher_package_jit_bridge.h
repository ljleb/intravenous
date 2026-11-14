#pragma once
#include <intravenous/bridge.h>
namespace iv {
class PackageWatcher;
class PackageJit;
IV_DECLARE_BRIDGE(package_watcher_package_jit_bridge, PackageWatcher, PackageJit);
}
