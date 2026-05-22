#include "runtime/project_service_timeline_bridge.h"

#include "runtime/project_service_events.h"
#include "runtime/timeline.h"

#include <unordered_map>

namespace iv {
namespace {
Timeline *bound_timeline = nullptr;

Timeline *bound_timeline_or_null()
{
    return bound_timeline;
}

void handle_live_input_snapshots_requested(
    std::vector<RuntimeProjectLiveInputSnapshotRequest> const &requests,
    RuntimeProjectLiveInputSnapshotsBuilder &builder)
{
    auto *timeline = bound_timeline_or_null();
    if (timeline == nullptr) {
        return;
    }

    std::vector<RuntimeProjectLiveInputSnapshot> snapshots;
    snapshots.reserve(requests.size());
    for (auto const &request : requests) {
        auto snapshot = RuntimeProjectLiveInputSnapshot{
            .logical_node_id = request.logical_node_id,
            .member_ordinal = request.member_ordinal,
            .input_ordinal = request.input_ordinal,
            .current_value = request.member_ordinal.has_value()
                ? timeline->live_input_value_or(
                    request.logical_node_id,
                    *request.member_ordinal,
                    request.input_ordinal,
                    request.fallback)
                : timeline->live_input_value_or(
                    request.logical_node_id,
                    request.input_ordinal,
                    request.fallback),
            .has_concrete_override = request.member_ordinal.has_value()
                ? timeline->has_live_input_value_override(
                    request.logical_node_id,
                    *request.member_ordinal,
                    request.input_ordinal)
                : false,
        };
        snapshots.push_back(std::move(snapshot));
    }
    builder.succeed(std::move(snapshots));
}

void handle_graph_input_lane_bindings_ensured(
    RuntimeProjectGraphInputLaneBindingsRequest const &request,
    RuntimeProjectAckBuilder &builder)
{
    auto *timeline = bound_timeline_or_null();
    if (timeline == nullptr) {
        return;
    }
    timeline->ensure_graph_input_lane_bindings(request.ports);
    builder.succeed();
}

void handle_graph_input_lane_bindings_requested(
    RuntimeProjectGraphInputLaneBindingsRequest const &request,
    RuntimeProjectGraphInputLaneBindingsBuilder &builder)
{
    auto *timeline = bound_timeline_or_null();
    if (timeline == nullptr) {
        return;
    }
    builder.succeed(timeline->reconcile_graph_input_lane_bindings(request.ports));
}

void handle_lane_outputs_requested(
    RuntimeProjectLaneOutputsRequest const &request,
    RuntimeProjectLaneOutputsBuilder &builder)
{
    auto *timeline = bound_timeline_or_null();
    if (timeline == nullptr) {
        return;
    }

    std::vector<RuntimeProjectLaneOutputs> results;
    results.reserve(request.lanes.size());
    for (auto const lane : request.lanes) {
        results.push_back(RuntimeProjectLaneOutputs{
            .lane = lane,
            .outputs = timeline->lane_outputs_for(lane),
        });
    }
    builder.succeed(std::move(results));
}

void handle_set_sample_input_value_requested(
    RuntimeProjectSetSampleInputValueRequest const &request,
    RuntimeProjectAckBuilder &builder)
{
    auto *timeline = bound_timeline_or_null();
    if (timeline == nullptr) {
        return;
    }

    if (request.member_ordinal.has_value()) {
        timeline->set_live_input_value(
            request.node_id,
            *request.member_ordinal,
            request.input_ordinal,
            request.value);
    } else {
        timeline->set_live_input_value(
            request.node_id,
            request.input_ordinal,
            request.value);
    }
    timeline->set_graph_input_sample_value(request.graph_input_port, request.value);
    builder.succeed();
}

void handle_clear_sample_input_value_override_requested(
    RuntimeProjectClearSampleInputValueOverrideRequest const &request,
    RuntimeProjectAckBuilder &builder)
{
    auto *timeline = bound_timeline_or_null();
    if (timeline == nullptr) {
        return;
    }

    timeline->clear_live_input_value_override(
        request.node_id,
        request.member_ordinal,
        request.input_ordinal);
    timeline->restore_graph_input_sample_inheritance(request.graph_input_port);
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

void bind_project_service_timeline_bridge(Timeline &timeline)
{
    bound_timeline = &timeline;
}

void unbind_project_service_timeline_bridge(Timeline const &timeline)
{
    if (bound_timeline == &timeline) {
        bound_timeline = nullptr;
    }
}
} // namespace iv
