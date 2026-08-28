#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleDefinitions;
class IvModuleReload;

IV_DECLARE_BRIDGE(
    iv_module_definitions_iv_module_reload_bridge,
    IvModuleDefinitions,
    IvModuleReload);
} // namespace iv
