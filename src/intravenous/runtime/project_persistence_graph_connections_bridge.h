#pragma once

#include <intravenous/bridge.h>

namespace iv {
class GraphConnections;
class ProjectPersistence;

IV_DECLARE_BRIDGE(
    project_persistence_graph_connections_bridge,
    ProjectPersistence,
    GraphConnections);
} // namespace iv
