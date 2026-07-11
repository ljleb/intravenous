#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
using ProjectPersistenceSaveRequestedEvent =
    void (*)(ProjectAckBuilder &builder);
using ProjectPersistenceCollectStateEvent =
    void (*)(ProjectPersistenceBuilder &builder);

IV_DECLARE_LINKER_EVENT(
    ProjectPersistenceSaveRequestedEvent,
    iv_runtime_project_persistence_save_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectPersistenceCollectStateEvent,
    iv_runtime_project_persistence_collect_state_event);
} // namespace iv
