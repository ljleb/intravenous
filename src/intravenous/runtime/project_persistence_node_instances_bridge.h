#pragma once

#include <intravenous/bridge.h>

namespace iv {
class NodeInstances;
class ProjectPersistence;

IV_DECLARE_BRIDGE(
    project_persistence_node_instances_bridge,
    ProjectPersistence,
    NodeInstances);
} // namespace iv
