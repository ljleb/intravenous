#pragma once
#include <intravenous/bridge.h>
namespace iv {
class IvPackageReload;
class IvPackageDefinitions;
IV_DECLARE_BRIDGE(
    iv_package_reload_iv_package_definitions_bridge,
    IvPackageReload,
    IvPackageDefinitions);
}
