#include <intravenous/runtime/package_watcher_package_definitions_bridge.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/package_pipeline_events.h>
namespace iv {
IV_DEFINE_BRIDGE(package_watcher_package_definitions_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_watcher_package_definitions_bridge,
    iv_runtime_package_refresh_event,
    &PackageDefinitions::handle_package_refresh)
}
