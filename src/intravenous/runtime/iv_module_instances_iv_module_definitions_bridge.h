#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleDefinitions;
class IvModuleInstances;

IV_DECLARE_BRIDGE(
    iv_module_instances_iv_module_definitions_bridge,
    IvModuleInstances,
    IvModuleDefinitions);
} // namespace iv
