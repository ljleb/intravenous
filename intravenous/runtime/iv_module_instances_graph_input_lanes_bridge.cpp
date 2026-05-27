#include "runtime/iv_module_instances_graph_input_lanes_bridge.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/iv_module_instances_events.h"

namespace iv {
namespace {
GraphInputLanes *bound_lanes = nullptr;

void handle_instances_changed(IvModuleInstancesChanged const &diff)
{
    if (bound_lanes == nullptr) {
        return;
    }
    bound_lanes->handle_iv_module_instances_changed(diff);
}

void handle_sample_input_resolution_requested(
    IvModuleSampleInputResolutionRequest const &request,
    IvModuleSampleInputResolutionBuilder &builder)
{
    if (bound_lanes == nullptr) {
        return;
    }
    builder.succeed(bound_lanes->resolve_sample_input_lane_ref(
        GraphInputLanesSampleInputLaneRefRequest{
            .logical_node_id = request.logical_node_id,
            .member_ordinal = request.member_ordinal,
            .input_ordinal = request.input_ordinal,
            .input_name = request.input_name,
            .default_value = request.default_value,
        }));
}

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleInstancesChangedEvent,
    iv_runtime_iv_module_instances_changed_event,
    handle_instances_changed);
IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleSampleInputResolutionRequestedEvent,
    iv_runtime_iv_module_sample_input_resolution_requested_event,
    handle_sample_input_resolution_requested);
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
