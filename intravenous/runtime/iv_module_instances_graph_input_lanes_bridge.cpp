#include "runtime/iv_module_instances_graph_input_lanes_bridge.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/iv_module_instances_events.h"

namespace iv {
namespace {
GraphInputLanes *bound_lanes = nullptr;

void handle_instances_changed(IvModuleInstancesChanged const &diff)
{
    (void)diff;
}

void handle_instance_builders_changed(IvModuleInstanceBuildersChanged const &diff)
{
    if (bound_lanes == nullptr) {
        return;
    }
    bound_lanes->handle_iv_module_instance_builders_changed(diff);
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleInstancesChangedEvent,
    iv_runtime_iv_module_instances_changed_event,
    handle_instances_changed);
IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleInstanceBuildersChangedEvent,
    iv_runtime_iv_module_instance_builders_changed_event,
    handle_instance_builders_changed);
} // namespace

void bind_iv_module_instances_graph_input_lanes_bridge(
    GraphInputLanes &lanes)
{
    bound_lanes = &lanes;
}

void unbind_iv_module_instances_graph_input_lanes_bridge(
    GraphInputLanes const &lanes)
{
    if (bound_lanes == &lanes) {
        bound_lanes = nullptr;
    }
}
} // namespace iv
