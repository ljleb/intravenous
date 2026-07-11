#pragma once

namespace iv {
class IvModuleDefinitions;

void bind_iv_module_reload_iv_module_definitions_bridge(
    IvModuleDefinitions &definitions);
void unbind_iv_module_reload_iv_module_definitions_bridge(
    IvModuleDefinitions const &definitions);
} // namespace iv
