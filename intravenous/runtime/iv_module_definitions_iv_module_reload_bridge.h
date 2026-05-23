#pragma once

namespace iv {
class RuntimeIvModuleReload;

void bind_iv_module_definitions_iv_module_reload_bridge(
    RuntimeIvModuleReload &reload);
void unbind_iv_module_definitions_iv_module_reload_bridge(
    RuntimeIvModuleReload const &reload);
} // namespace iv
