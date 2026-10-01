#pragma once
#include <intravenous/bridge.h>
namespace iv {
class PackageJit;
class ProjectPersistence;
IV_DECLARE_BRIDGE(project_persistence_package_jit_bridge, ProjectPersistence, PackageJit);
}
