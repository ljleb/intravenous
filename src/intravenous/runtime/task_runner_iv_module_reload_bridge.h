#pragma once

namespace iv {
class IvModuleReload;

void bind_task_runner_iv_module_reload_bridge(IvModuleReload &reload);
void unbind_task_runner_iv_module_reload_bridge(IvModuleReload const &reload);
} // namespace iv
