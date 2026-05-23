#pragma once

namespace iv {
class RuntimeIvModuleInstances;

void bind_iv_module_definitions_iv_module_instances_bridge(
    RuntimeIvModuleInstances &instances);
void unbind_iv_module_definitions_iv_module_instances_bridge(
    RuntimeIvModuleInstances const &instances);
} // namespace iv
