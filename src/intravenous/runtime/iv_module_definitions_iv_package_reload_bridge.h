#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleDefinitions;
class IvPackageReload;

IV_DECLARE_BRIDGE(
    iv_module_definitions_iv_package_reload_bridge,
    IvModuleDefinitions,
    IvPackageReload);
} // namespace iv
