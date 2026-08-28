#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ProjectPersistence;
class Timeline;

IV_DECLARE_BRIDGE(project_persistence_timeline_bridge, ProjectPersistence, Timeline);
} // namespace iv
