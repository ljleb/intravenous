#include <intravenous/runtime/project_persistence_package_jit_bridge.h>
#include <intravenous/runtime/package_jit.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>
namespace iv {
IV_DEFINE_BRIDGE(project_persistence_package_jit_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_package_jit_bridge,
    iv_runtime_project_override_settings_requested_event,
    &PackageJit::handle_project_override_settings)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_package_jit_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &PackageJit::handle_project_persistence_collect_state)
}
