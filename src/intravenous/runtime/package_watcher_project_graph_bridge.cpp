#include <intravenous/runtime/package_watcher_project_graph_bridge.h>
#include <intravenous/runtime/package_watcher.h>
#include <intravenous/runtime/project_graph_events.h>
namespace iv {
IV_DEFINE_BRIDGE(package_watcher_project_graph_bridge)
IV_SUBSCRIBE_LINKER_EVENT(package_watcher_project_graph_bridge,
    iv_runtime_project_graph_execution_published_event, &PackageWatcher::handle_execution_publication)
}
