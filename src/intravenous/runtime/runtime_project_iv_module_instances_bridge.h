#pragma once

namespace iv {
class IvModuleInstances;

void bind_runtime_project_iv_module_instances_bridge(IvModuleInstances &iv_module_instances);
void unbind_runtime_project_iv_module_instances_bridge(IvModuleInstances const &iv_module_instances);
} // namespace iv
