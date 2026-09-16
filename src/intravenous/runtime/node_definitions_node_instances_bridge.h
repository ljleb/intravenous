#pragma once

#include <intravenous/bridge.h>

namespace iv {
class NodeDefinitions;
class NodeInstances;

IV_DECLARE_BRIDGE(
    node_definitions_node_instances_bridge,
    NodeDefinitions,
    NodeInstances);
} // namespace iv
