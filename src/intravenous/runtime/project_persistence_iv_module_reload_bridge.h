#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleReload;
class ProjectPersistence;

IV_DECLARE_BRIDGE(project_persistence_iv_module_reload_bridge, ProjectPersistence, IvModuleReload);
} // namespace iv
