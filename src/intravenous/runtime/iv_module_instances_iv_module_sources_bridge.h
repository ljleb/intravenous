#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleInstances;
class IvModuleSources;

IV_DECLARE_BRIDGE(
    iv_module_instances_iv_module_sources_bridge,
    IvModuleInstances,
    IvModuleSources);
} // namespace iv
