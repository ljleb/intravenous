#pragma once

#include <intravenous/bridge.h>

namespace iv {
class GraphExecutor;
class ProjectGraph;

IV_DECLARE_BRIDGE(
    project_graph_graph_executor_bridge,
    ProjectGraph,
    GraphExecutor);
} // namespace iv
