#pragma once

#include <intravenous/bridge.h>

namespace iv {
class NodeInstances;
class ProjectGraph;

IV_DECLARE_BRIDGE(
    project_graph_node_instances_bridge,
    ProjectGraph,
    NodeInstances);
} // namespace iv
