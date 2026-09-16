#pragma once

#include <intravenous/bridge.h>

namespace iv {
class NodeDefinitions;
class IvModuleInstances;

IV_DECLARE_BRIDGE(
    node_definitions_iv_module_instances_bridge,
    NodeDefinitions,
    IvModuleInstances);
} // namespace iv
