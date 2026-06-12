#include <intravenous/runtime/graph_input_lanes_iv_module_instances_execution_bridge.h>

#include <intravenous/runtime/graph_input_lanes_execution_edges_events.h>
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

void handle_graph_input_lanes_dsp_task_dependencies_changed(
    GraphInputLanesDspTaskDependenciesChanged const &changed)
{
    if (!bound_execution) {
        return;
    }
    maybe_publish(bound_execution->handle_graph_input_lanes_dsp_task_dependencies_changed(changed));
}

IV_SUBSCRIBE_LINKER_EVENT(
    GraphInputLanesDspTaskDependenciesChangedEvent,
    iv_runtime_graph_input_lanes_dsp_task_dependencies_changed_event,
    handle_graph_input_lanes_dsp_task_dependencies_changed);
}

void bind_graph_input_lanes_iv_module_instances_execution_bridge(
    IvModuleInstancesExecution &execution)
{
    bound_execution = &execution;
}

void unbind_graph_input_lanes_iv_module_instances_execution_bridge(
    IvModuleInstancesExecution const &execution)
{
    if (bound_execution == &execution) {
        bound_execution = nullptr;
    }
}
}
