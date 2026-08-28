#pragma once

#include <intravenous/bridge.h>

namespace iv {
class GraphInputLanes;
class ProjectPersistence;

IV_DECLARE_BRIDGE(
    project_persistence_graph_input_lanes_bridge,
    ProjectPersistence,
    GraphInputLanes);
} // namespace iv
