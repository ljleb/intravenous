#include <intravenous/runtime/package_watcher_service_bridge.h>
#include <intravenous/runtime/package_pipeline_events.h>
#include <intravenous/runtime/package_watcher.h>
namespace iv {
IV_DEFINE_BRIDGE(package_watcher_service_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_watcher_service_bridge,
    iv_runtime_package_watcher_refresh_requested_event,
    &PackageWatcher::handle_refresh_requested)
}
