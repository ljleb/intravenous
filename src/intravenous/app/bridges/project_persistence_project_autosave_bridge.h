#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ProjectAutosave;
class ProjectPersistence;

IV_DECLARE_BRIDGE(
    project_persistence_project_autosave_bridge,
    ProjectPersistence,
    ProjectAutosave);
} // namespace iv
