#pragma once

namespace iv {
class IvModuleReload;

void bind_runtime_project_iv_module_reload_bridge(IvModuleReload &iv_module_reload);
void unbind_runtime_project_iv_module_reload_bridge(IvModuleReload const &iv_module_reload);
} // namespace iv
