#pragma once

namespace iv {
class IvModuleDefinitions;

void bind_iv_module_definitions_builder_bridge(
    IvModuleDefinitions &definitions);
void unbind_iv_module_definitions_builder_bridge(
    IvModuleDefinitions const &definitions);
} // namespace iv
