#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvPackageReload;
class ProjectPersistence;

IV_DECLARE_BRIDGE(project_persistence_iv_package_reload_bridge, ProjectPersistence, IvPackageReload);
} // namespace iv
