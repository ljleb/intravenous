#pragma once

namespace iv {
class RuntimeIvModuleDefinitions;

void bind_iv_module_instances_iv_module_definitions_bridge(
    RuntimeIvModuleDefinitions &definitions);
void unbind_iv_module_instances_iv_module_definitions_bridge(
    RuntimeIvModuleDefinitions const &definitions);
} // namespace iv
