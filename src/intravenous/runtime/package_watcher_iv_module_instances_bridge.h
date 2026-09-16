#pragma once
#include <intravenous/bridge.h>
namespace iv {
class PackageWatcher;
class IvModuleInstances;
IV_DECLARE_BRIDGE(
    package_watcher_iv_module_instances_bridge,
    PackageWatcher,
    IvModuleInstances);
}
