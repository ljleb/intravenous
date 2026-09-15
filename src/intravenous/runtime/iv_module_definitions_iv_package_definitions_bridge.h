#pragma once
#include <intravenous/bridge.h>
namespace iv {
class IvModuleDefinitions;
class IvPackageDefinitions;
IV_DECLARE_BRIDGE(
    iv_module_definitions_iv_package_definitions_bridge,
    IvModuleDefinitions,
    IvPackageDefinitions);
}
