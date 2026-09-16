#include <intravenous/runtime/package_reload_iv_package_definitions_bridge.h>
#include <intravenous/runtime/iv_package_definitions.h>
#include <intravenous/runtime/package_reload_events.h>
namespace iv {
IV_DEFINE_BRIDGE(package_reload_iv_package_definitions_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_reload_iv_package_definitions_bridge,
    iv_runtime_package_build_statuses_changed_event,
    &IvPackageDefinitions::handle_package_build_statuses_changed)
}
