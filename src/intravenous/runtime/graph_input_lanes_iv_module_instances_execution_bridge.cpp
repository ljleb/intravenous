#include <intravenous/runtime/graph_input_lanes_iv_module_instances_execution_bridge.h>

#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/iv_module_instances_execution_events.h>

namespace iv {
namespace {
IvModuleInstancesExecution *bound_execution = nullptr;

void maybe_publish(VersionedTaskGraphUpdate const &update)
{
    if (update.update.to_create.empty()
        && update.update.to_update.empty()
        && update.update.to_delete.empty()) {
        return;
    }
    IV_INVOKE_LINKER_EVENT(iv_runtime_iv_module_instances_execution_tasks_changed_event, update);
}

void handle_timeline_batch_requested(
    TimelineLaneBatchUpdate const &batch,
    GraphInputLanesAckBuilder &)
{
    if (!bound_execution) {
        return;
    }
    maybe_publish(bound_execution->handle_timeline_batch(batch));
}

IV_SUBSCRIBE_LINKER_EVENT(
    GraphInputLanesTimelineBatchRequestedEvent,
    iv_runtime_graph_input_lanes_timeline_batch_requested_event,
    handle_timeline_batch_requested);
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
