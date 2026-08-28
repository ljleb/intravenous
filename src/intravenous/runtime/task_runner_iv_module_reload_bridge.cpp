#include <intravenous/runtime/task_runner_iv_module_reload_bridge.h>

#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/task_runner_events.h>

namespace iv {
IV_DEFINE_BRIDGE(task_runner_iv_module_reload_bridge)
IV_SUBSCRIBE_BRIDGE(
    task_runner_iv_module_reload_bridge,
    iv_runtime_task_runner_before_pass_event,
    &IvModuleReload::handle_task_runner_before_pass);
} // namespace iv
