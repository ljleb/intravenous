#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleDefinitions;
class IvModuleSourceIntrospection;

IV_DECLARE_BRIDGE(
    iv_module_definitions_iv_module_source_introspection_bridge,
    IvModuleDefinitions,
    IvModuleSourceIntrospection);
} // namespace iv
