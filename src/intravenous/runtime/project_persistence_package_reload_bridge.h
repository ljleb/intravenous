#pragma once

#include <intravenous/bridge.h>

namespace iv {
class PackageReload;
class ProjectPersistence;

IV_DECLARE_BRIDGE(project_persistence_package_reload_bridge, ProjectPersistence, PackageReload);
} // namespace iv
