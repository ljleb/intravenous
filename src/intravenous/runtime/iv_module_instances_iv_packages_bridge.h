#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleInstances;
class IvPackages;

IV_DECLARE_BRIDGE(
    iv_module_instances_iv_packages_bridge,
    IvModuleInstances,
    IvPackages);
} // namespace iv
