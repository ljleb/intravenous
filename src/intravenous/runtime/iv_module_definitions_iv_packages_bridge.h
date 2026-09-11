#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleDefinitions;
class IvPackages;

IV_DECLARE_BRIDGE(
    iv_module_definitions_iv_packages_bridge,
    IvModuleDefinitions,
    IvPackages);
} // namespace iv
