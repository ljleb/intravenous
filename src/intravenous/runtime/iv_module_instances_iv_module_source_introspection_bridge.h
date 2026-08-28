#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleInstances;
class IvModuleSourceIntrospection;

IV_DECLARE_BRIDGE(
    iv_module_instances_iv_module_source_introspection_bridge,
    IvModuleInstances,
    IvModuleSourceIntrospection);
} // namespace iv
