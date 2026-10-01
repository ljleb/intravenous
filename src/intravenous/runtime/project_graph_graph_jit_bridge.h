#pragma once

#include <intravenous/bridge.h>

namespace iv {
class GraphJit;
class ProjectGraph;

IV_DECLARE_BRIDGE(
    project_graph_graph_jit_bridge,
    ProjectGraph,
    GraphJit);
} // namespace iv
