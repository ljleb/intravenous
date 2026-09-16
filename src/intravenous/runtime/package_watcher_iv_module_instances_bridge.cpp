#include <intravenous/runtime/package_watcher_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/package_watcher.h>
namespace iv {
IV_DEFINE_BRIDGE(package_watcher_iv_module_instances_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_watcher_iv_module_instances_bridge,
    iv_runtime_iv_module_required_definitions_changed_event,
    &PackageWatcher::handle_required_definitions_changed)
}
