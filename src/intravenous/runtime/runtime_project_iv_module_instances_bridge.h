#pragma once

namespace iv {
class IvModuleInstances;
class IvModuleSources;

void bind_runtime_project_iv_module_instances_bridge(
    IvModuleInstances &iv_module_instances,
    IvModuleSources &sources);
void unbind_runtime_project_iv_module_instances_bridge(
    IvModuleInstances const &iv_module_instances,
    IvModuleSources const &sources);
} // namespace iv
