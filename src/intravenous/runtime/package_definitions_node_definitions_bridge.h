#pragma once
#include <intravenous/bridge.h>
namespace iv {
class PackageDefinitions;
class NodeDefinitions;
IV_DECLARE_BRIDGE(
    package_definitions_node_definitions_bridge,
    PackageDefinitions,
    NodeDefinitions);
}
