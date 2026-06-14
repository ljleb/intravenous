#include <intravenous/runtime/graph_input_lanes_iv_module_instances_bridge.h>

#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/iv_module_instances.h>

namespace iv {
namespace {
IvModuleInstances *bound_instances = nullptr;

void handle_graph_input_lanes_rebuild_requested(
    GraphInputLanesRebuildRequested const &request)
{
    if (bound_instances == nullptr) {
        return;
    }
    bound_instances->handle_graph_input_lanes_rebuild_requested(request);
}

IV_SUBSCRIBE_LINKER_EVENT(
    GraphInputLanesRebuildRequestedEvent,
    iv_runtime_graph_input_lanes_rebuild_requested_event,
    handle_graph_input_lanes_rebuild_requested);
} // namespace

void bind_graph_input_lanes_iv_module_instances_bridge(
    IvModuleInstances &instances)
{
    bound_instances = &instances;
}

void unbind_graph_input_lanes_iv_module_instances_bridge(
    IvModuleInstances const &instances)
{
    if (bound_instances == &instances) {
        bound_instances = nullptr;
    }
}
} // namespace iv
