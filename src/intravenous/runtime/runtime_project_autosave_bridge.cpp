#include <intravenous/runtime/runtime_project_autosave_bridge.h>

#include <intravenous/runtime/project_autosave.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
namespace {
ProjectAutosave* bound_project_autosave = nullptr;

IV_SUBSCRIBE_LINKER_EVENT(
    ProjectStateChangedEvent,
    iv_runtime_project_state_changed_event,
    [] {
        if (bound_project_autosave != nullptr) {
            bound_project_autosave->project_state_changed();
        }
    });
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectLoadedEvent,
    iv_runtime_project_loaded_event,
    [] {
        if (bound_project_autosave != nullptr) {
            bound_project_autosave->project_loaded();
        }
    });
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetAutosaveEnabledRequestedEvent,
    iv_runtime_project_set_autosave_enabled_requested_event,
    [](ProjectSetAutosaveEnabledRequest const& request, ProjectAckBuilder& builder) {
        if (bound_project_autosave == nullptr) {
            return;
        }
        bound_project_autosave->set_enabled(request.enabled);
        builder.succeed();
    });
} // namespace

void bind_runtime_project_autosave_bridge(ProjectAutosave& project_autosave)
{
    bound_project_autosave = &project_autosave;
}

void unbind_runtime_project_autosave_bridge(ProjectAutosave const& project_autosave)
{
    if (bound_project_autosave == &project_autosave) {
        bound_project_autosave = nullptr;
    }
}
} // namespace iv
