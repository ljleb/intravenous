#include <intravenous/runtime/project_persistence_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    ProjectPersistenceSaveRequestedEvent,
    iv_runtime_project_persistence_save_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectPersistenceCollectStateEvent,
    iv_runtime_project_persistence_collect_state_event)
} // namespace iv
