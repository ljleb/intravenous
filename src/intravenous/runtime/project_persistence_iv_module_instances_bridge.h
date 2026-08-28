#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleInstances;
class ProjectPersistence;

IV_DECLARE_BRIDGE(
    project_persistence_iv_module_instances_bridge,
    ProjectPersistence,
    IvModuleInstances);
} // namespace iv
