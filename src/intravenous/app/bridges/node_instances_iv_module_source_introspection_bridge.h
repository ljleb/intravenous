#pragma once

#include <intravenous/bridge.h>

namespace iv {
class NodeInstances;
class IvModuleSourceIntrospection;

IV_DECLARE_BRIDGE(
    node_instances_iv_module_source_introspection_bridge,
    NodeInstances,
    IvModuleSourceIntrospection);
} // namespace iv
