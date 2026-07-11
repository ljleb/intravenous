#pragma once

namespace iv {
class IvModuleReload;

void bind_iv_module_definitions_iv_module_reload_bridge(
    IvModuleReload &reload);
void unbind_iv_module_definitions_iv_module_reload_bridge(
    IvModuleReload const &reload);
} // namespace iv
