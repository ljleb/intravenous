#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ProjectPersistence;
class TimelineExecution;

IV_DECLARE_BRIDGE(
    project_persistence_timeline_execution_bridge,
    ProjectPersistence,
    TimelineExecution);
} // namespace iv
