#include <intravenous/runtime/project_persistence_project_autosave_bridge.h>

#include <intravenous/runtime/project_autosave.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_project_autosave_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_autosave_bridge,
    iv_runtime_project_state_changed_event,
    &ProjectAutosave::handle_project_state_changed)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_autosave_bridge,
    iv_runtime_project_loaded_event,
    &ProjectAutosave::handle_project_loaded)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_project_autosave_bridge,
    iv_runtime_project_set_autosave_enabled_requested_event,
    &ProjectAutosave::handle_project_set_autosave_enabled)
} // namespace iv
