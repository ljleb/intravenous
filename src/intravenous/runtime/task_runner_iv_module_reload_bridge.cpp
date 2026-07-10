#include <intravenous/runtime/task_runner_iv_module_reload_bridge.h>

#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/task_runner_events.h>

namespace iv {
namespace {
IvModuleReload *bound_reload = nullptr;

void handle_task_runner_before_pass(TasksRunnerBeforePass const &)
{
    if (bound_reload == nullptr || !bound_reload->has_pending_results()) {
        return;
    }
    bound_reload->apply_pending_results();
}

IV_SUBSCRIBE_LINKER_EVENT(
    TasksRunnerBeforePassEvent,
    iv_runtime_task_runner_before_pass_event,
    handle_task_runner_before_pass);
} // namespace

void bind_task_runner_iv_module_reload_bridge(IvModuleReload &reload)
{
    bound_reload = &reload;
}

void unbind_task_runner_iv_module_reload_bridge(IvModuleReload const &reload)
{
    if (bound_reload == &reload) {
        bound_reload = nullptr;
    }
}
} // namespace iv
