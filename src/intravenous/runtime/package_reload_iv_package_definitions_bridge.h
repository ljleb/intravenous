#pragma once
#include <intravenous/bridge.h>
namespace iv {
class PackageReload;
class IvPackageDefinitions;
IV_DECLARE_BRIDGE(
    package_reload_iv_package_definitions_bridge,
    PackageReload,
    IvPackageDefinitions);
}
