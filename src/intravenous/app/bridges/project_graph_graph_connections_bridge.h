#pragma once

#include <intravenous/bridge.h>

namespace iv {
class GraphConnections;
class ProjectGraph;

IV_DECLARE_BRIDGE(
    project_graph_graph_connections_bridge,
    ProjectGraph,
    GraphConnections);
} // namespace iv
