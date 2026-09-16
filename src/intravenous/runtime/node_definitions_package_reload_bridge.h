#pragma once

#include <intravenous/bridge.h>

namespace iv {
class NodeDefinitions;
class PackageReload;

IV_DECLARE_BRIDGE(
    node_definitions_package_reload_bridge,
    NodeDefinitions,
    PackageReload);
} // namespace iv
