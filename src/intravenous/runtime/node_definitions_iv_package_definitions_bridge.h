#pragma once
#include <intravenous/bridge.h>
namespace iv {
class NodeDefinitions;
class IvPackageDefinitions;
IV_DECLARE_BRIDGE(
    node_definitions_iv_package_definitions_bridge,
    NodeDefinitions,
    IvPackageDefinitions);
}
