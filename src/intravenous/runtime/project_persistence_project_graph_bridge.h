#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ProjectGraph;
class ProjectPersistence;

IV_DECLARE_BRIDGE(
    project_persistence_project_graph_bridge,
    ProjectPersistence,
    ProjectGraph);
} // namespace iv
