#pragma once

namespace iv {
class IvModuleInstances;

void bind_iv_module_definitions_iv_module_instances_bridge(
    IvModuleInstances &instances);
void unbind_iv_module_definitions_iv_module_instances_bridge(
    IvModuleInstances const &instances);
} // namespace iv
