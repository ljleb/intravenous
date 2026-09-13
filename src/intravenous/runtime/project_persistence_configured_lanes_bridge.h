#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ConfiguredLanes;
class ProjectPersistence;

IV_DECLARE_BRIDGE(
    project_persistence_configured_lanes_bridge,
    ProjectPersistence,
    ConfiguredLanes);
} // namespace iv
