#include "runtime/graph_input_lanes_timeline_bridge.h"

#include "runtime/graph_input_lanes_events.h"
#include "runtime/timeline.h"
#include "runtime/timeline_events.h"

#include <ranges>
#include <unordered_set>

namespace iv {
namespace {
Timeline *bound_timeline = nullptr;

void emit_lane_change(RuntimeTimelineLanesChanged change)
{
    IV_INVOKE_LINKER_EVENT(iv_runtime_timeline_lanes_changed_event, change);
}

std::string graph_input_port_key(GraphInputPortDescriptor const &port)
{
    std::string key = port.logical_node_id;
    key += "\x1fmember:";
    key += port.concrete_member_ordinal.has_value()
        ? std::to_string(*port.concrete_member_ordinal)
        : "logical";
    key += "\x1fkind:";
    key += port.port_kind == PortKind::sample ? "sample" : "event";
    key += "\x1fordinal:";
    key += std::to_string(port.port_ordinal);
    return key;
}

void prune_stale_graph_input_lanes(
    Timeline &timeline,
    std::vector<GraphInputPortDescriptor> const &ports)
{
    std::unordered_set<std::string> live_ports;
    live_ports.reserve(ports.size());
    for (auto const &port : ports) {
        live_ports.insert(graph_input_port_key(port));
    }

    std::vector<LaneId> to_remove;
    for (auto const lane_id : timeline.lane_ids()) {
        auto const metadata = timeline.lane_metadata(lane_id);
        if (!metadata.graph_input_port.has_value()) {
            continue;
        }
        if (std::ranges::find(metadata.tags, "graph-input") == metadata.tags.end()) {
            continue;
        }
        if (live_ports.contains(graph_input_port_key(*metadata.graph_input_port))) {
            continue;
        }
        to_remove.push_back(lane_id);
    }

    for (auto const lane_id : to_remove) {
        timeline.remove_lane(lane_id);
    }
}

void handle_ports_changed(
    RuntimeGraphInputLanesPortsChangedRequest const &request,
    RuntimeGraphInputLanesAckBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }
    prune_stale_graph_input_lanes(*bound_timeline, request.ports);
    bound_timeline->ensure_graph_input_lane_bindings(request.ports);
    emit_lane_change(RuntimeTimelineLanesChanged{
        .lane_set_changed = true,
    });
    builder.succeed();
}

void handle_live_input_snapshots_requested(
    std::vector<RuntimeGraphInputLanesLiveInputSnapshotRequest> const &requests,
    RuntimeGraphInputLanesLiveInputSnapshotsBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }

    std::vector<RuntimeGraphInputLanesLiveInputSnapshot> snapshots;
    snapshots.reserve(requests.size());
    for (auto const &request : requests) {
        snapshots.push_back(RuntimeGraphInputLanesLiveInputSnapshot{
            .logical_node_id = request.logical_node_id,
            .member_ordinal = request.member_ordinal,
            .input_ordinal = request.input_ordinal,
            .current_value = request.member_ordinal.has_value()
                ? bound_timeline->live_input_value_or(
                    request.logical_node_id,
                    *request.member_ordinal,
                    request.input_ordinal,
                    request.fallback)
                : bound_timeline->live_input_value_or(
                    request.logical_node_id,
                    request.input_ordinal,
                    request.fallback),
            .has_concrete_override = request.member_ordinal.has_value()
                ? bound_timeline->has_live_input_value_override(
                    request.logical_node_id,
                    *request.member_ordinal,
                    request.input_ordinal)
                : false,
        });
    }
    builder.succeed(std::move(snapshots));
}

void handle_lane_bindings_ensured(
    RuntimeGraphInputLanesPortsChangedRequest const &request,
    RuntimeGraphInputLanesAckBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }
    bound_timeline->ensure_graph_input_lane_bindings(request.ports);
    emit_lane_change(RuntimeTimelineLanesChanged{
        .lane_set_changed = true,
    });
    builder.succeed();
}

void handle_lane_bindings_requested(
    RuntimeGraphInputLanesPortsChangedRequest const &request,
    RuntimeGraphInputLanesLaneBindingsBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }
    builder.succeed(bound_timeline->reconcile_graph_input_lane_bindings(request.ports));
}

void handle_lane_outputs_requested(
    std::vector<LaneId> const &lanes,
    RuntimeGraphInputLanesLaneOutputsBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }

    std::vector<RuntimeGraphInputLanesLaneOutputs> results;
    results.reserve(lanes.size());
    for (auto const lane : lanes) {
        results.push_back(RuntimeGraphInputLanesLaneOutputs{
            .lane = lane,
            .outputs = bound_timeline->lane_outputs_for(lane),
        });
    }
    builder.succeed(std::move(results));
}

void handle_set_sample_input_value_requested(
    RuntimeGraphInputLanesSetSampleInputValueRequest const &request,
    RuntimeGraphInputLanesAckBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }
    if (request.member_ordinal.has_value()) {
        bound_timeline->set_live_input_value(
            request.node_id,
            *request.member_ordinal,
            request.input_ordinal,
            request.value);
    } else {
        bound_timeline->set_live_input_value(
            request.node_id,
            request.input_ordinal,
            request.value);
    }
    bound_timeline->set_graph_input_sample_value(request.graph_input_port, request.value);
    emit_lane_change(RuntimeTimelineLanesChanged{
        .lane_set_changed = true,
    });
    builder.succeed();
}

void handle_sample_input_lane_ref_requested(
    RuntimeGraphInputLanesSampleInputLaneRefRequest const &request,
    RuntimeGraphInputLanesSampleInputLaneRefBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }
    auto const lane = bound_timeline->resolve_graph_sample_input_lane(
        request.logical_node_id,
        request.member_ordinal,
        request.input_ordinal,
        request.input_name,
        request.default_value);
    if (!lane) {
        return;
    }
    builder.succeed(RealtimeLaneRef(*bound_timeline, lane));
}

void handle_clear_sample_input_value_override_requested(
    RuntimeGraphInputLanesClearSampleInputValueOverrideRequest const &request,
    RuntimeGraphInputLanesAckBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }
    bound_timeline->clear_live_input_value_override(
        request.node_id,
        request.member_ordinal,
        request.input_ordinal);
    bound_timeline->restore_graph_input_sample_inheritance(request.graph_input_port);
    emit_lane_change(RuntimeTimelineLanesChanged{
        .lane_set_changed = true,
    });
    builder.succeed();
}

IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeGraphInputLanesPortsChangedEvent,
    iv_runtime_graph_input_lanes_ports_changed_event,
    handle_ports_changed);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeGraphInputLanesLiveInputSnapshotsRequestedEvent,
    iv_runtime_graph_input_lanes_live_input_snapshots_requested_event,
    handle_live_input_snapshots_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeGraphInputLanesLaneBindingsEnsuredEvent,
    iv_runtime_graph_input_lanes_lane_bindings_ensured_event,
    handle_lane_bindings_ensured);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeGraphInputLanesLaneBindingsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_bindings_requested_event,
    handle_lane_bindings_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeGraphInputLanesLaneOutputsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_outputs_requested_event,
    handle_lane_outputs_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeGraphInputLanesSetSampleInputValueRequestedEvent,
    iv_runtime_graph_input_lanes_set_sample_input_value_requested_event,
    handle_set_sample_input_value_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeGraphInputLanesSampleInputLaneRefRequestedEvent,
    iv_runtime_graph_input_lanes_sample_input_lane_ref_requested_event,
    handle_sample_input_lane_ref_requested);
IV_SUBSCRIBE_LINKER_EVENT(
    RuntimeGraphInputLanesClearSampleInputValueOverrideRequestedEvent,
    iv_runtime_graph_input_lanes_clear_sample_input_value_override_requested_event,
    handle_clear_sample_input_value_override_requested);
} // namespace

void bind_graph_input_lanes_timeline_bridge(Timeline &timeline)
{
    bound_timeline = &timeline;
}

void unbind_graph_input_lanes_timeline_bridge(Timeline const &timeline)
{
    if (bound_timeline == &timeline) {
        bound_timeline = nullptr;
    }
}
} // namespace iv
