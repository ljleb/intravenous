#pragma once
#include <intravenous/bridge.h>
namespace iv {
class PackageWatcher;
class NodeInstances;
IV_DECLARE_BRIDGE(
    package_watcher_node_instances_bridge,
    PackageWatcher,
    NodeInstances);
}
