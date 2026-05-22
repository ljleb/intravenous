#include "runtime/project_service_graph_input_lanes_bridge.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/project_service_events.h"

namespace iv {
namespace {
RuntimeGraphInputLanes *bound_lanes = nullptr;

void handle_live_input_snapshots_requested(
    std::vector<RuntimeProjectLiveInputSnapshotRequest> const &requests,
    RuntimeProjectLiveInputSnapshotsBuilder &builder)
{
    if (bound_lanes == nullptr) {
        return;
    }
    builder.succeed(bound_lanes->collect_live_input_snapshots(requests));
}

void handle_graph_input_lane_bindings_ensured(
    RuntimeProjectGraphInputLaneBindingsRequest const &request,
    RuntimeProjectAckBuilder &builder)
{
    if (bound_lanes == nullptr) {
        return;
    }
    bound_lanes->ensure_graph_input_lane_bindings(request);
    builder.succeed();
}

void handle_graph_input_lane_bindings_requested(
    RuntimeProjectGraphInputLaneBindingsRequest const &request,
    RuntimeProjectGraphInputLaneBindingsBuilder &builder)
{
    if (bound_lanes == nullptr) {
        return;
    }
    builder.succeed(bound_lanes->query_graph_input_lane_bindings(request));
}

void handle_lane_outputs_requested(
    RuntimeProjectLaneOutputsRequest const &request,
    RuntimeProjectLaneOutputsBuilder &builder)
{
    if (bound_lanes == nullptr) {
        return;
    }
    builder.succeed(bound_lanes->query_lane_outputs(request));
}

void handle_set_sample_input_value_requested(
    RuntimeProjectSetSampleInputValueRequest const &request,
    RuntimeProjectAckBuilder &builder)
{
    if (bound_lanes == nullptr) {
        return;
    }
    bound_lanes->set_sample_input_value(request);
    builder.succeed();
}

void handle_clear_sample_input_value_override_requested(
    RuntimeProjectClearSampleInputValueOverrideRequest const &request,
    RuntimeProjectAckBuilder &builder)
{
    if (bound_lanes == nullptr) {
        return;
    }
    bound_lanes->clear_sample_input_value_override(request);
    builder.succeed();
}

IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeProjectLiveInputSnapshotsRequestedEvent,
    iv_runtime_project_live_input_snapshots_requested_event,
    handle_live_input_snapshots_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeProjectGraphInputLaneBindingsEnsuredEvent,
    iv_runtime_project_graph_input_lane_bindings_ensured_event,
    handle_graph_input_lane_bindings_ensured);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeProjectGraphInputLaneBindingsRequestedEvent,
    iv_runtime_project_graph_input_lane_bindings_requested_event,
    handle_graph_input_lane_bindings_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeProjectLaneOutputsRequestedEvent,
    iv_runtime_project_lane_outputs_requested_event,
    handle_lane_outputs_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeProjectSetSampleInputValueRequestedEvent,
    iv_runtime_project_set_sample_input_value_requested_event,
    handle_set_sample_input_value_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeProjectClearSampleInputValueOverrideRequestedEvent,
    iv_runtime_project_clear_sample_input_value_override_requested_event,
    handle_clear_sample_input_value_override_requested);
} // namespace

void bind_project_service_graph_input_lanes_bridge(RuntimeGraphInputLanes &lanes)
{
    bound_lanes = &lanes;
}

void unbind_project_service_graph_input_lanes_bridge(
    RuntimeGraphInputLanes const &lanes)
{
    if (bound_lanes == &lanes) {
        bound_lanes = nullptr;
    }
}
} // namespace iv
