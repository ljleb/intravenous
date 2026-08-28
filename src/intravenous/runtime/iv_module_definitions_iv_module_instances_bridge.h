#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleDefinitions;
class IvModuleInstances;

IV_DECLARE_BRIDGE(
    iv_module_definitions_iv_module_instances_bridge,
    IvModuleDefinitions,
    IvModuleInstances);
} // namespace iv
