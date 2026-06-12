#include <intravenous/runtime/iv_module_instances_iv_module_instances_execution_bridge.h>

#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_execution_events.h>

namespace iv {
namespace {
IvModuleInstancesExecution *bound_execution = nullptr;

void maybe_publish(TaskGraphUpdate const &update)
{
    if (update.to_create.empty() && update.to_update.empty() && update.to_delete.empty()) {
        return;
    }
    IV_INVOKE_LINKER_EVENT(iv_runtime_iv_module_instances_execution_tasks_changed_event, update);
}

void handle_iv_module_instance_builders_completed(
    IvModuleInstanceBuildersChanged const &changed)
{
    if (!bound_execution) {
        return;
    }
    maybe_publish(bound_execution->handle_instance_builders_changed(changed));
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleInstanceBuildersCompletedEvent,
    iv_runtime_iv_module_instance_builders_completed_event,
    handle_iv_module_instance_builders_completed);
}

void bind_iv_module_instances_iv_module_instances_execution_bridge(
    IvModuleInstancesExecution &execution)
{
    bound_execution = &execution;
}

void unbind_iv_module_instances_iv_module_instances_execution_bridge(
    IvModuleInstancesExecution const &execution)
{
    if (bound_execution == &execution) {
        bound_execution = nullptr;
    }
}
}
