#include "runtime/graph_input_lanes.h"

#include "runtime/graph_input_lanes_events.h"

#include <stdexcept>

namespace iv {
namespace {
void append_graph_input_port_descriptors(
    std::vector<GraphInputPortDescriptor> &ports,
    std::string const &logical_node_id,
    std::optional<size_t> concrete_member_ordinal,
    PortKind port_kind,
    std::span<LogicalPortInfo const> logical_ports)
{
    ports.reserve(ports.size() + logical_ports.size());
    for (auto const &port : logical_ports) {
        ports.push_back(GraphInputPortDescriptor{
            .logical_node_id = logical_node_id,
            .concrete_member_ordinal = concrete_member_ordinal,
            .port_kind = port_kind,
            .port_ordinal = port.ordinal,
            .port_name = port.name,
            .port_type = port.type,
        });
    }
}
} // namespace

std::vector<GraphInputPortDescriptor>
RuntimeGraphInputLanes::graph_input_port_descriptors_for(
    std::span<IntrospectionLogicalNode const> logical_nodes)
{
    std::vector<GraphInputPortDescriptor> ports;
    for (auto const &node : logical_nodes) {
        append_graph_input_port_descriptors(
            ports, node.id, std::nullopt, PortKind::sample, node.sample_inputs);
        append_graph_input_port_descriptors(
            ports, node.id, std::nullopt, PortKind::event, node.event_inputs);
        for (auto const &member : node.members) {
            append_graph_input_port_descriptors(
                ports,
                node.id,
                member.ordinal,
                PortKind::sample,
                member.sample_inputs);
            append_graph_input_port_descriptors(
                ports,
                node.id,
                member.ordinal,
                PortKind::event,
                member.event_inputs);
        }
    }
    return ports;
}

LaneQueryResult RuntimeGraphInputLanes::query_lanes_locked(
    LaneQueryFilter const &filter,
    std::optional<size_t> start_index,
    std::optional<size_t> visible_lane_count)
{
    if (filter.kind != "graphInputs") {
        throw std::runtime_error("unsupported lane filter: " + filter.kind);
    }

    LaneQueryResult result;
    RuntimeGraphInputLanesLaneBindingsBuilder lane_bindings_builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_lane_bindings_requested_event,
        RuntimeGraphInputLanesPortsChangedRequest{ .ports = current_ports },
        lane_bindings_builder);
    auto const controls = lane_bindings_builder.build();

    std::vector<LaneInfo> lane_infos;
    lane_infos.reserve(
        controls.logical_sample_knobs.size() +
        controls.sample_inputs.size() +
        controls.event_inputs.size());
    for (auto const &control : controls.logical_sample_knobs) {
        lane_infos.push_back(LaneInfo{
            .lane_id = control.knob_lane.value,
            .domain = LaneDomain::realtime,
            .graph_input_port = control.port,
        });
    }
    for (auto const &control : controls.sample_inputs) {
        lane_infos.push_back(LaneInfo{
            .lane_id = control.knob_lane.value,
            .domain = LaneDomain::realtime,
            .graph_input_port = control.port,
        });
    }
    for (auto const &event_input : controls.event_inputs) {
        lane_infos.push_back(LaneInfo{
            .lane_id = event_input.graph_input_lane.value,
            .domain = LaneDomain::realtime,
            .graph_input_port = event_input.port,
        });
    }

    result.total_lane_count = lane_infos.size();
    result.start_index = start_index.value_or(0);
    result.visible_lane_count = visible_lane_count.value_or(result.total_lane_count);

    size_t const max_start_index =
        result.total_lane_count == 0
            ? 0
            : (result.visible_lane_count >= result.total_lane_count
                   ? 0
                   : result.total_lane_count - result.visible_lane_count);
    size_t const lane_begin = std::min(result.start_index, max_start_index);
    size_t const remaining_lanes = result.total_lane_count - lane_begin;
    size_t const window_lane_count =
        std::min(result.visible_lane_count, remaining_lanes);
    size_t const lane_end = lane_begin + window_lane_count;
    bool const restrict_connections_to_window =
        start_index.has_value() || visible_lane_count.has_value();
    result.start_index = lane_begin;
    result.lanes.reserve(window_lane_count);

    std::unordered_set<uint64_t> visible_lane_ids;
    visible_lane_ids.reserve(window_lane_count);
    for (size_t lane_index = lane_begin; lane_index < lane_end; ++lane_index) {
        auto const &lane = lane_infos[lane_index];
        visible_lane_ids.insert(lane.lane_id);
        result.lanes.push_back(lane);
    }

    for (auto const &lane : lane_infos) {
        if (!visible_lane_ids.contains(lane.lane_id) &&
            restrict_connections_to_window) {
            continue;
        }
    }

    std::vector<LaneId> requested_output_lanes;
    requested_output_lanes.reserve(lane_infos.size());
    for (auto const &lane : lane_infos) {
        if (!visible_lane_ids.contains(lane.lane_id) &&
            restrict_connections_to_window) {
            continue;
        }
        requested_output_lanes.push_back(LaneId{lane.lane_id});
    }

    RuntimeGraphInputLanesLaneOutputsBuilder lane_outputs_builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_lane_outputs_requested_event,
        requested_output_lanes,
        lane_outputs_builder);
    auto const lane_outputs = lane_outputs_builder.build();

    for (auto const &lane_output_group : lane_outputs) {
        LaneId const source = lane_output_group.lane;
        for (auto const &output : lane_output_group.outputs) {
            if (restrict_connections_to_window &&
                !visible_lane_ids.contains(source.value) &&
                !visible_lane_ids.contains(output.target.value)) {
                continue;
            }
            result.connections.push_back(LaneConnectionInfo{
                .source_lane_id = source.value,
                .target_lane_id = output.target.value,
                .port_kind = output.input.kind,
                .port_ordinal = output.input.ordinal,
            });
        }
    }

    return result;
}

void RuntimeGraphInputLanes::handle_iv_modules_definitions_changed(
    RuntimeIvModuleDefinitionsChanged const &diff)
{
    auto const changed_count = diff.added.size() + diff.updated.size();
    if (changed_count == 0) {
        if (!diff.removed_definition_ids.empty()) {
            std::scoped_lock lock(mutex);
            current_ports.clear();
            RuntimeGraphInputLanesAckBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_graph_input_lanes_ports_changed_event,
                RuntimeGraphInputLanesPortsChangedRequest{ .ports = current_ports },
                builder);
            builder.build();
        }
        return;
    }
    if (changed_count > 1 || (!diff.added.empty() && !diff.updated.empty())) {
        throw std::runtime_error(
            "multiple iv-module definition changes are not yet supported");
    }

    RuntimeIvModuleDefinition const &definition =
        !diff.added.empty() ? diff.added.front() : diff.updated.front();

    std::scoped_lock lock(mutex);
    current_ports = graph_input_port_descriptors_for(
        definition.introspection.logical_nodes);
    RuntimeGraphInputLanesAckBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_ports_changed_event,
        RuntimeGraphInputLanesPortsChangedRequest{ .ports = current_ports },
        builder);
    builder.build();
}

std::vector<RuntimeProjectLiveInputSnapshot>
RuntimeGraphInputLanes::collect_live_input_snapshots(
    std::vector<RuntimeProjectLiveInputSnapshotRequest> const &requests)
{
    std::vector<RuntimeGraphInputLanesLiveInputSnapshotRequest> lane_requests;
    lane_requests.reserve(requests.size());
    for (auto const &request : requests) {
        lane_requests.push_back(RuntimeGraphInputLanesLiveInputSnapshotRequest{
            .logical_node_id = request.logical_node_id,
            .member_ordinal = request.member_ordinal,
            .input_ordinal = request.input_ordinal,
            .fallback = request.fallback,
        });
    }

    RuntimeGraphInputLanesLiveInputSnapshotsBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_live_input_snapshots_requested_event,
        lane_requests,
        builder);
    auto const lane_snapshots = builder.build();

    std::vector<RuntimeProjectLiveInputSnapshot> snapshots;
    snapshots.reserve(lane_snapshots.size());
    for (auto const &snapshot : lane_snapshots) {
        snapshots.push_back(RuntimeProjectLiveInputSnapshot{
            .logical_node_id = snapshot.logical_node_id,
            .member_ordinal = snapshot.member_ordinal,
            .input_ordinal = snapshot.input_ordinal,
            .current_value = snapshot.current_value,
            .has_concrete_override = snapshot.has_concrete_override,
        });
    }
    return snapshots;
}

void RuntimeGraphInputLanes::ensure_graph_input_lane_bindings(
    RuntimeProjectGraphInputLaneBindingsRequest const &request)
{
    RuntimeGraphInputLanesAckBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_lane_bindings_ensured_event,
        RuntimeGraphInputLanesPortsChangedRequest{ .ports = request.ports },
        builder);
    builder.build();
}

GraphInputLaneBindings RuntimeGraphInputLanes::query_graph_input_lane_bindings(
    RuntimeProjectGraphInputLaneBindingsRequest const &request)
{
    RuntimeGraphInputLanesLaneBindingsBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_lane_bindings_requested_event,
        RuntimeGraphInputLanesPortsChangedRequest{ .ports = request.ports },
        builder);
    return builder.build();
}

std::vector<RuntimeProjectLaneOutputs> RuntimeGraphInputLanes::query_lane_outputs(
    RuntimeProjectLaneOutputsRequest const &request)
{
    RuntimeGraphInputLanesLaneOutputsBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_lane_outputs_requested_event,
        request.lanes,
        builder);
    auto const lane_outputs = builder.build();

    std::vector<RuntimeProjectLaneOutputs> results;
    results.reserve(lane_outputs.size());
    for (auto const &lane_output : lane_outputs) {
        results.push_back(RuntimeProjectLaneOutputs{
            .lane = lane_output.lane,
            .outputs = lane_output.outputs,
        });
    }
    return results;
}

void RuntimeGraphInputLanes::set_sample_input_value(
    RuntimeProjectSetSampleInputValueRequest const &request)
{
    std::scoped_lock lock(mutex);
    RuntimeGraphInputLanesAckBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_set_sample_input_value_requested_event,
        RuntimeGraphInputLanesSetSampleInputValueRequest{
            .node_id = request.node_id,
            .member_ordinal = request.member_ordinal,
            .input_ordinal = request.input_ordinal,
            .value = request.value,
            .graph_input_port = request.graph_input_port,
        },
        builder);
    builder.build();
}

void RuntimeGraphInputLanes::clear_sample_input_value_override(
    RuntimeProjectClearSampleInputValueOverrideRequest const &request)
{
    std::scoped_lock lock(mutex);
    RuntimeGraphInputLanesAckBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_clear_sample_input_value_override_requested_event,
        RuntimeGraphInputLanesClearSampleInputValueOverrideRequest{
            .node_id = request.node_id,
            .member_ordinal = request.member_ordinal,
            .input_ordinal = request.input_ordinal,
            .graph_input_port = request.graph_input_port,
        },
        builder);
    builder.build();
}

LaneQueryResult RuntimeGraphInputLanes::query_lanes(
    LaneQueryFilter const &filter,
    std::optional<size_t> start_index,
    std::optional<size_t> visible_lane_count)
{
    std::scoped_lock lock(mutex);
    return query_lanes_locked(filter, start_index, visible_lane_count);
}
} // namespace iv
