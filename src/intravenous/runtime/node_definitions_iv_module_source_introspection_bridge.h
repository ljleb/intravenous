#pragma once

#include <intravenous/bridge.h>

namespace iv {
class NodeDefinitions;
class IvModuleSourceIntrospection;

IV_DECLARE_BRIDGE(
    node_definitions_iv_module_source_introspection_bridge,
    NodeDefinitions,
    IvModuleSourceIntrospection);
} // namespace iv
