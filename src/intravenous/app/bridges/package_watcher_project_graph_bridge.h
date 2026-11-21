#pragma once
#include <intravenous/bridge.h>
namespace iv {
class PackageWatcher;
class ProjectGraph;
IV_DECLARE_BRIDGE(package_watcher_project_graph_bridge, PackageWatcher, ProjectGraph);
}
