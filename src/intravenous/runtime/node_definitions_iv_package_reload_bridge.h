#pragma once

#include <intravenous/bridge.h>

namespace iv {
class NodeDefinitions;
class IvPackageReload;

IV_DECLARE_BRIDGE(
    node_definitions_iv_package_reload_bridge,
    NodeDefinitions,
    IvPackageReload);
} // namespace iv
