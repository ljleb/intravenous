#include <intravenous/runtime/iv_module_instances_iv_packages_bridge.h>

#include <intravenous/runtime/iv_packages.h>
#include <intravenous/runtime/iv_packages_events.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_instances_iv_packages_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_instances_iv_packages_bridge,
    iv_runtime_iv_package_lookup_event,
    &IvPackages::handle_iv_package_lookup)
} // namespace iv
