#pragma once

#include <intravenous/bridge.h>

namespace iv {
class NodeDefinitions;
class ProjectGraph;

IV_DECLARE_BRIDGE(
    node_definitions_project_graph_bridge,
    NodeDefinitions,
    ProjectGraph);
} // namespace iv
