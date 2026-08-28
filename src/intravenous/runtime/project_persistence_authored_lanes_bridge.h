#pragma once

#include <intravenous/bridge.h>

namespace iv {
class AuthoredLanes;
class ProjectPersistence;

IV_DECLARE_BRIDGE(
    project_persistence_authored_lanes_bridge,
    ProjectPersistence,
    AuthoredLanes);
} // namespace iv
