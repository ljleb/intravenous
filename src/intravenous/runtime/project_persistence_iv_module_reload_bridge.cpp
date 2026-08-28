#include <intravenous/runtime/project_persistence_iv_module_reload_bridge.h>

#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_iv_module_reload_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_iv_module_reload_bridge,
    iv_runtime_project_override_settings_requested_event,
    &IvModuleReload::handle_project_override_settings);
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_iv_module_reload_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &IvModuleReload::handle_project_persistence_collect_state);
} // namespace iv
