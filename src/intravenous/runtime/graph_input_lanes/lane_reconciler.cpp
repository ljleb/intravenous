#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes/details.h>

#include <intravenous/basic_nodes/routing.h>
#include <intravenous/runtime/task_ids.h>

namespace iv {
using namespace graph_input_lanes_details;

Sample& GraphInputLanes::ensure_live_input_value_locked(
    std::string_view key,
    size_t input_ordinal
)
{
    auto &values = live_input_values[std::string(key)];
    if (values.size() <= input_ordinal) {
        values.resize(input_ordinal + 1);
    }
    if (!values[input_ordinal]) {
        values[input_ordinal] = Sample { 0.0f };
    }
    return *values[input_ordinal];
}

Sample& GraphInputLanes::ensure_live_input_value_initialized_locked(
    std::string_view key,
    size_t input_ordinal,
    Sample initial_value
)
{
    auto &values = live_input_values[std::string(key)];
    if (values.size() <= input_ordinal) {
        values.resize(input_ordinal + 1);
    }
    if (!values[input_ordinal]) {
        values[input_ordinal] = initial_value;
    }
    return *values[input_ordinal];
}

Sample GraphInputLanes::live_input_value_or_locked(
    std::string_view virtual_node_id,
    size_t input_ordinal,
    Sample fallback) const
{
    auto it = live_input_values.find(std::string(virtual_node_id));
    if (it == live_input_values.end() || it->second.size() <= input_ordinal) {
        return fallback;
    }
    if (!it->second[input_ordinal]) {
        return fallback;
    }
    return *it->second[input_ordinal];
}

Sample GraphInputLanes::live_input_value_or_locked(
    std::string_view virtual_node_id,
    size_t member_ordinal,
    size_t input_ordinal,
    Sample fallback) const
{
    auto const override = node_bundle_override_key(virtual_node_id, member_ordinal, input_ordinal);
    auto const key = node_bundle_key(virtual_node_id, member_ordinal);
    if (node_bundle_live_input_overrides.contains(override)) {
        auto it = live_input_values.find(key);
        if (it != live_input_values.end()
            && it->second.size() > input_ordinal
            && it->second[input_ordinal]) {
            return *it->second[input_ordinal];
        }
    }
    return live_input_value_or_locked(virtual_node_id, input_ordinal, fallback);
}

void GraphInputLanes::schedule_instances_for_input_locked(
    std::string_view virtual_node_id,
    std::optional<size_t> member_ordinal,
    size_t input_ordinal)
{
    std::unordered_set<std::string> instance_ids;
    for (auto const &port : desired_ports) {
        if (port.port.port_kind != PortKind::sample) {
            continue;
        }
        if (port.port.virtual_node_id != virtual_node_id) {
            continue;
        }
        if (port.port.port_ordinal != input_ordinal) {
            continue;
        }
        if (member_ordinal.has_value()) {
            if (port.port.node_bundle_port_ordinal != member_ordinal) {
                continue;
            }
        }
        instance_ids.insert(port.instance_id);
    }
    for (auto const& instance_id : instance_ids)
        schedule_runtime_binding_sync_locked(instance_id);
}

void GraphInputLanes::schedule_instances_for_virtual_sample_input_locked(
    std::string_view virtual_node_id,
    size_t input_ordinal)
{
    schedule_instances_for_input_locked(
        virtual_node_id,
        std::nullopt,
        input_ordinal);
}

std::optional<GraphInputLanes::DesiredGraphPort> GraphInputLanes::find_desired_port_locked(
    std::string const &instance_id,
    GraphInputPortDescriptor const &port) const
{
    auto const it = desired_ports_by_instance_id.find(instance_id);
    if (it == desired_ports_by_instance_id.end()) {
        return std::nullopt;
    }
    for (auto const &desired : it->second) {
        if (desired.port == port) {
            return desired;
        }
    }
    return std::nullopt;
}

void GraphInputLanes::refresh_desired_ports_locked()
{
    desired_ports.clear();
    for (auto const &[_, ports] : desired_ports_by_instance_id) {
        desired_ports.insert(
            desired_ports.end(),
            ports.begin(),
            ports.end());
    }
}

void GraphInputLanes::refresh_desired_output_ports_locked()
{
    desired_output_ports.clear();
    for (auto const &[_, ports] : desired_output_ports_by_instance_id) {
        desired_output_ports.insert(
            desired_output_ports.end(),
            ports.begin(),
            ports.end());
    }
}

void GraphInputLanes::refresh_desired_public_input_ports_locked()
{
    desired_public_input_ports.clear();
    for (auto const &[_, ports] : desired_public_input_ports_by_instance_id) {
        desired_public_input_ports.insert(
            desired_public_input_ports.end(),
            ports.begin(),
            ports.end());
    }
}

void GraphInputLanes::refresh_desired_public_output_ports_locked()
{
    desired_public_output_ports.clear();
    for (auto const &[_, ports] : desired_public_output_ports_by_instance_id) {
        desired_public_output_ports.insert(
            desired_public_output_ports.end(),
            ports.begin(),
            ports.end());
    }
}

void GraphInputLanes::reconcile_public_ports_locked(TimelineLaneBatchUpdate *batch)
{
    std::unordered_map<std::string, ExistingTrackedLane> public_lanes_by_key;
    std::unordered_set<std::uint64_t> used_lane_ids;
    size_t desired_input_count = desired_public_input_ports.size();
    size_t desired_output_count = desired_public_output_ports.size();
    size_t created_count = 0;
    size_t reused_count = 0;
    size_t removed_count = 0;

    for (auto const &tracked : tracked_lanes) {
        lane_ids.observe(tracked.lane);
        if (!tracked.metadata.has_unit(metadata_public)) {
            continue;
        }
        public_lanes_by_key.emplace(public_port_identity_key(DesiredPublicGraphPort{
            .module_instance_id =
                tracked.metadata.int_value(metadata_module_instance_id).value_or(0),
            .input = tracked.metadata.has_unit(metadata_public_input),
            .port_kind =
                tracked.metadata.int_value(metadata_port_kind).value_or(0) == 0
                    ? PortKind::sample
                    : PortKind::event,
            .port_ordinal =
                static_cast<size_t>(tracked.metadata.int_value(metadata_port_ordinal).value_or(0)),
            .sample_channel_type = [&]() -> std::optional<ChannelTypeId> {
                auto const channel = tracked.metadata.int_value(metadata_channel_type);
                if (!channel.has_value()) {
                    return std::nullopt;
                }
                return static_cast<ChannelTypeId>(*channel);
            }(),
            .source_identity_hash = tracked.metadata.int_value(metadata_public_source_id),
            .public_port_name_hash = tracked.metadata.int_value(metadata_public_port_name),
            .node_bundle_port_ordinal = [&]() -> std::optional<size_t> {
                auto const member = tracked.metadata.int_value(metadata_node_bundle_port_ordinal);
                if (!member.has_value() || *member < 0) {
                    return std::nullopt;
                }
                return static_cast<size_t>(*member);
            }(),
        }), tracked);
    }

    auto reconcile_port = [&](DesiredPublicGraphPort const &port) {
        auto const key = public_port_key(port);
        auto const metadata = public_graph_port_metadata(
            port,
            port.port_kind == PortKind::sample,
            port.port_kind == PortKind::event);
        auto const make_upsert = [&](LaneId lane, InternedString external_id) {
            return TimelineLaneUpsert{
                .lane = lane,
                .external_id = std::move(external_id),
                .make_node = [port, lane] {
                    if (port.input) {
                        if (port.port_kind == PortKind::event) {
                            return TypeErasedLaneNode(GraphEventInputLaneNode{});
                        }
                        return make_sample_input_node(
                            port.default_value,
                            port.port_name.empty() ? "public sample input" : port.port_name);
                    }
                    if (port.port_kind == PortKind::event) {
                        return TypeErasedLaneNode(GraphEventOutputLaneNode{ .lane = lane });
                    }
                    return TypeErasedLaneNode(GraphSampleOutputLaneNode{
                        .lane = lane,
                        .name = port.port_name.empty() ? "public sample output" : port.port_name,
                    });
                },
                .sample_channel_type = port.port_kind == PortKind::sample
                    ? port.sample_channel_type
                    : std::nullopt,
                .metadata = metadata,
                .external_task_dependencies = port.input
                    ? std::vector<std::string>{}
                    : std::vector<std::string>{
                        iv_module_instance_dsp_task_id(port.instance_id),
                    },
            };
        };
        LaneId lane {};
        if (auto const it = public_lanes_by_key.find(key); it != public_lanes_by_key.end()) {
            lane = it->second.lane;
            auto const existing_channel_type = it->second.metadata.int_value(metadata_channel_type);
            auto const desired_channel_type = port.sample_channel_type.has_value()
                ? std::optional<int>(static_cast<int>(*port.sample_channel_type))
                : std::nullopt;
            if (existing_channel_type != desired_channel_type) {
                ++removed_count;
                ++created_count;
                if (batch != nullptr) {
                    batch->removals.push_back(lane);
                    batch->upserts.push_back(
                        make_upsert(lane, it->second.external_id));
                }
            } else {
                ++reused_count;
            }
        } else {
            lane = stable_lane_id_for_key(key);
            ++created_count;
            if (batch != nullptr) {
                auto const external_id_state_key = port.source_identity.empty()
                    ? std::string{}
                    : public_sample_input_state_key(
                        port.instance_id,
                        port.source_identity,
                        port.node_bundle_port_ordinal);
                auto const &lane_ids_by_key = port.port_kind == PortKind::event
                    ? public_event_input_lane_ids_by_key
                    : public_sample_input_lane_ids_by_key;
                auto const external_id_it = lane_ids_by_key.find(external_id_state_key);
                batch->upserts.push_back(make_upsert(
                    lane,
                    external_id_it != lane_ids_by_key.end()
                        ? external_id_it->second
                        : InternedString::from_string(public_port_external_id(port))));
            }
        }
        used_lane_ids.insert(lane.value);
    };

    std::unordered_set<std::string> reconciled_public_port_keys;
    for (auto const &port : desired_public_input_ports) {
        // Inputs without a source annotation retain the legacy one-port/one-
        // lane behavior.  Annotated inputs add a virtual lane keyed by source
        // identity and, on demand, a lane for an individual evaluation.
        if (port.source_identity.empty()) {
            if (reconciled_public_port_keys.insert(public_port_key(port)).second) {
                reconcile_port(port);
            }
            continue;
        }

        auto const virtual_state_key = public_sample_input_state_key(
            port.instance_id, port.source_identity, std::nullopt);
        auto const virtual_is_timeline = [&] {
            if (port.port_kind == PortKind::event) {
                auto const it = public_event_input_states_by_key.find(virtual_state_key);
                return it == public_event_input_states_by_key.end()
                    || it->second == ProjectEventInputState::timeline_lane;
            }
            auto const it = public_sample_input_states_by_key.find(virtual_state_key);
            return it == public_sample_input_states_by_key.end()
                || it->second == ProjectSampleInputState::timeline_lane;
        }();
        if (virtual_is_timeline
            && reconciled_public_port_keys.insert(public_port_key(port)).second) {
            reconcile_port(port);
        }

        auto const member_state_key = public_sample_input_state_key(
            port.instance_id, port.source_identity, port.port_ordinal);
        auto const member_is_timeline = [&] {
            if (port.port_kind == PortKind::event) {
                auto const it = public_event_input_states_by_key.find(member_state_key);
                return it != public_event_input_states_by_key.end()
                    && it->second == ProjectEventInputState::timeline_lane;
            }
            auto const it = public_sample_input_states_by_key.find(member_state_key);
            return it != public_sample_input_states_by_key.end()
                && it->second == ProjectSampleInputState::timeline_lane;
        }();
        if (!member_is_timeline) {
            continue;
        }
        auto node_bundle_port = port;
        node_bundle_port.node_bundle_port_ordinal = port.port_ordinal;
        if (reconciled_public_port_keys.insert(public_port_key(node_bundle_port)).second) {
            reconcile_port(node_bundle_port);
        }
    }
    for (auto const &port : desired_public_output_ports) {
        if (port.source_identity.empty()) {
            if (reconciled_public_port_keys.insert(public_port_key(port)).second) reconcile_port(port);
            continue;
        }
        auto const virtual_key = public_sample_input_state_key(
            port.instance_id, port.source_identity, std::nullopt);
        auto const member_key = public_sample_input_state_key(
            port.instance_id, port.source_identity, port.port_ordinal);
        bool connected = false;
        if (port.port_kind == PortKind::sample) {
            auto const virtual_it = public_sample_output_states_by_key.find(virtual_key);
            auto const virtual_connected = virtual_it == public_sample_output_states_by_key.end()
                || virtual_it->second == ProjectSampleOutputState::timeline_lane;
            auto const member_it = public_sample_output_states_by_key.find(member_key);
            auto const member_state = member_it == public_sample_output_states_by_key.end()
                ? ProjectSampleOutputState::virtual_port : member_it->second;
            connected = member_state == ProjectSampleOutputState::timeline_lane
                || (member_state == ProjectSampleOutputState::virtual_port && virtual_connected);
        } else {
            auto const virtual_it = public_event_output_states_by_key.find(virtual_key);
            auto const virtual_connected = virtual_it == public_event_output_states_by_key.end()
                || virtual_it->second == ProjectEventOutputState::timeline_lane;
            auto const member_it = public_event_output_states_by_key.find(member_key);
            auto const member_state = member_it == public_event_output_states_by_key.end()
                ? ProjectEventOutputState::virtual_port : member_it->second;
            connected = member_state == ProjectEventOutputState::timeline_lane
                || (member_state == ProjectEventOutputState::virtual_port && virtual_connected);
        }
        if (connected && reconciled_public_port_keys.insert(public_port_key(port)).second) {
            reconcile_port(port);
        }
    }

    if (batch != nullptr) {
        for (auto const &tracked : tracked_lanes) {
            if (!tracked.metadata.has_unit(metadata_public)) {
                continue;
            }
            if (!used_lane_ids.contains(tracked.lane.value)) {
                ++removed_count;
                batch->removals.push_back(tracked.lane);
            }
        }
    }

    if (batch != nullptr && (desired_input_count > 0 || desired_output_count > 0 ||
        created_count > 0 || removed_count > 0)) {
        emit_debug_message(
            "graph public ports reconciled: desiredInputs="
            + std::to_string(desired_input_count)
            + " desiredOutputs=" + std::to_string(desired_output_count)
            + " existingTracked=" + std::to_string(public_lanes_by_key.size())
            + " created=" + std::to_string(created_count)
            + " reused=" + std::to_string(reused_count)
            + " removed=" + std::to_string(removed_count));
    }
}

LaneId GraphInputLanes::public_graph_port_lane_for(DesiredPublicGraphPort const &port) const
{
    return stable_lane_id_for_key(public_port_key(port));
}

Sample& GraphInputLanes::ensure_public_sample_input_value_locked(
    std::string const& instance_id,
    std::string const& source_identity,
    Sample default_value
)
{
    auto const key = public_sample_input_state_key(instance_id, source_identity, std::nullopt);
    return public_sample_input_values.try_emplace(key, default_value).first->second;
}

std::optional<LaneId> GraphInputLanes::effective_public_sample_input_lane_locked(
    DesiredPublicGraphPort const &port) const
{
    if (port.port_kind != PortKind::sample || port.source_identity.empty()) {
        return public_graph_port_lane_for(port);
    }

    auto const virtual_state_key = public_sample_input_state_key(
        port.instance_id, port.source_identity, std::nullopt);
    auto const virtual_state_it = public_sample_input_states_by_key.find(virtual_state_key);
    auto const virtual_state = virtual_state_it == public_sample_input_states_by_key.end()
        ? ProjectSampleInputState::timeline_lane
        : virtual_state_it->second;

    auto const member_state_key = public_sample_input_state_key(
        port.instance_id, port.source_identity, port.port_ordinal);
    auto const member_state_it = public_sample_input_states_by_key.find(member_state_key);
    auto const member_state = member_state_it == public_sample_input_states_by_key.end()
        ? ProjectSampleInputState::virtual_follow
        : member_state_it->second;

    if (member_state == ProjectSampleInputState::disconnected) {
        return std::nullopt;
    }
    if (member_state == ProjectSampleInputState::timeline_lane) {
        auto node_bundle_port = port;
        node_bundle_port.node_bundle_port_ordinal = port.port_ordinal;
        return public_graph_port_lane_for(node_bundle_port);
    }
    if (virtual_state == ProjectSampleInputState::disconnected) {
        return std::nullopt;
    }
    if (virtual_state == ProjectSampleInputState::overridden) {
        return std::nullopt;
    }
    return public_graph_port_lane_for(port);
}

std::optional<LaneId> GraphInputLanes::effective_public_event_input_lane_locked(
    DesiredPublicGraphPort const &port) const
{
    if (port.port_kind != PortKind::event || port.source_identity.empty()) {
        return public_graph_port_lane_for(port);
    }
    auto const virtual_key = public_sample_input_state_key(port.instance_id, port.source_identity, std::nullopt);
    auto const virtual_it = public_event_input_states_by_key.find(virtual_key);
    auto const virtual_state = virtual_it == public_event_input_states_by_key.end()
        ? ProjectEventInputState::timeline_lane : virtual_it->second;
    auto const member_key = public_sample_input_state_key(port.instance_id, port.source_identity, port.port_ordinal);
    auto const member_it = public_event_input_states_by_key.find(member_key);
    auto const member_state = member_it == public_event_input_states_by_key.end()
        ? ProjectEventInputState::virtual_follow : member_it->second;
    if (member_state == ProjectEventInputState::disconnected || virtual_state == ProjectEventInputState::disconnected) {
        return std::nullopt;
    }
    if (member_state == ProjectEventInputState::timeline_lane) {
        auto concrete = port;
        concrete.node_bundle_port_ordinal = port.port_ordinal;
        return public_graph_port_lane_for(concrete);
    }
    return public_graph_port_lane_for(port);
}

std::optional<GraphInputLanes::NodeBundleOutputState>
GraphInputLanes::effective_node_bundle_output_state_locked(
    DesiredGraphPort const &port) const
{
    auto virtual_port = port;
    virtual_port.port.node_bundle_port_ordinal = std::nullopt;
    bool const virtual_is_timeline_lane =
        virtual_output_is_timeline_lane_locked(virtual_port);

    std::optional<NodeBundleOutputState> state =
        virtual_is_timeline_lane
            ? std::optional<NodeBundleOutputState>(NodeBundleOutputState::virtual_port)
            : std::nullopt;
    if (auto const it = node_bundle_output_states_by_key.find(graph_input_port_key(port.port));
        it != node_bundle_output_states_by_key.end()) {
        state = it->second;
    }
    return state;
}

bool GraphInputLanes::virtual_output_is_timeline_lane_locked(
    DesiredGraphPort const &port) const
{
    auto const it = virtual_output_states_by_key.find(graph_input_port_key(port.port));
    return it != virtual_output_states_by_key.end()
        && it->second == VirtualOutputState::timeline_lane;
}

void GraphInputLanes::reconcile_output_ports_locked(TimelineLaneBatchUpdate *batch)
{
    std::unordered_map<std::string, ExistingTrackedLane> virtual_outputs_by_key;
    std::unordered_map<std::string, ExistingTrackedLane> node_bundle_outputs_by_key;
    std::unordered_set<std::uint64_t> used_lane_ids;

    for (auto const &tracked : tracked_lanes) {
        lane_ids.observe(tracked.lane);
        if (!tracked.metadata.has_unit(metadata_graph_output)
            || tracked.metadata.has_unit(metadata_public)) {
            continue;
        }
        if (tracked.metadata.has_unit(metadata_virtual)) {
            virtual_outputs_by_key.emplace(
                existing_identity_key(tracked.metadata, "virtual-output"),
                tracked);
        } else if (tracked.metadata.has_unit(metadata_node_bundle)) {
            node_bundle_outputs_by_key.emplace(
                existing_identity_key(tracked.metadata, "node-bundle-output"),
                tracked);
        }
    }

    for (auto const &port : desired_output_ports) {
        if (port.port.node_bundle_port_ordinal.has_value()) {
            continue;
        }
        if (!virtual_output_is_timeline_lane_locked(port)) {
            continue;
        }

        bool const is_event = port.port.port_kind == PortKind::event;
        auto const key = output_identity_key(port, "virtual-output");
        LaneId lane {};
        if (auto const it = virtual_outputs_by_key.find(key); it != virtual_outputs_by_key.end()) {
            lane = it->second.lane;
        } else {
            lane = stable_lane_id_for_key(key);
            if (batch != nullptr) {
                auto metadata = graph_output_metadata(port, true, false, !is_event, is_event);
                auto external_id = lane_external_id_or_new(
                    virtual_output_lane_ids_by_key,
                    virtual_outputs_by_key,
                    key);
                batch->upserts.push_back(TimelineLaneUpsert{
                    .lane = lane,
                    .external_id = external_id,
                    .make_node = [lane, is_event] {
                        if (is_event) {
                            return TypeErasedLaneNode(GraphEventOutputLaneNode{ .lane = lane });
                        }
                        return TypeErasedLaneNode(GraphSampleOutputLaneNode{ .lane = lane });
                    },
                    .sample_channel_type = is_event
                        ? std::nullopt
                        : port.port.sample_channel_type,
                    .metadata = std::move(metadata),
                    .external_task_dependencies = {
                        iv_module_instance_dsp_task_id(port.instance_id),
                    },
                });
            }
        }
        used_lane_ids.insert(lane.value);
    }

    // Dedicated lanes: one per concrete output port whose effective state is timeline_lane.
    for (auto const &port : desired_output_ports) {
        if (!port.port.node_bundle_port_ordinal.has_value()) {
            continue;
        }
        auto const state = effective_node_bundle_output_state_locked(port);
        if (state != NodeBundleOutputState::timeline_lane) {
            continue;
        }

        bool const is_event = port.port.port_kind == PortKind::event;
        auto const key = output_identity_key(port, "node-bundle-output");
        LaneId lane {};
        if (auto const it = node_bundle_outputs_by_key.find(key); it != node_bundle_outputs_by_key.end()) {
            lane = it->second.lane;
        } else {
            lane = stable_lane_id_for_key(key);
            if (batch != nullptr) {
                auto metadata = graph_output_metadata(port, false, true, !is_event, is_event);
                auto external_id = lane_external_id_or_new(
                    node_bundle_output_lane_ids_by_key,
                    node_bundle_outputs_by_key,
                    key);
                batch->upserts.push_back(TimelineLaneUpsert{
                    .lane = lane,
                    .external_id = external_id,
                    .make_node = [lane, is_event] {
                        if (is_event) {
                            return TypeErasedLaneNode(GraphEventOutputLaneNode{ .lane = lane });
                        }
                        return TypeErasedLaneNode(GraphSampleOutputLaneNode{ .lane = lane });
                    },
                    .sample_channel_type = is_event
                        ? std::nullopt
                        : port.port.sample_channel_type,
                    .metadata = std::move(metadata),
                    .external_task_dependencies = {
                        iv_module_instance_dsp_task_id(port.instance_id),
                    },
                });
            }
        }
        used_lane_ids.insert(lane.value);
    }

    if (batch != nullptr) {
        for (auto const &tracked : tracked_lanes) {
            if (!tracked.metadata.has_unit(metadata_graph_output)
                || tracked.metadata.has_unit(metadata_public)) {
                continue;
            }
            if (!used_lane_ids.contains(tracked.lane.value)) {
                batch->removals.push_back(tracked.lane);
            }
        }
    }
}

LaneId GraphInputLanes::graph_output_lane_for(
    GraphInputPortDescriptor const &port,
    bool virtual_aggregation)
{
    std::scoped_lock lock(mutex);
    for (auto const &tracked : tracked_lanes) {
        if (!tracked.metadata.has_unit(metadata_graph_output)) {
            continue;
        }
        if (virtual_aggregation && !tracked.metadata.has_unit(metadata_virtual)) {
            continue;
        }
        if (!virtual_aggregation && !tracked.metadata.has_unit(metadata_node_bundle)) {
            continue;
        }
        if (tracked.metadata.int_value(metadata_virtual_node_id) != hash_string(port.virtual_node_id)) {
            continue;
        }
        if (tracked.metadata.int_value(metadata_port_kind)
            != static_cast<int>(port.port_kind == PortKind::event)) {
            continue;
        }
        if (tracked.metadata.int_value(metadata_port_ordinal) != static_cast<int>(port.port_ordinal)) {
            continue;
        }
        if (tracked.metadata.int_value(metadata_channel_type)
            != (port.sample_channel_type.has_value()
                ? std::optional<int>(static_cast<int>(*port.sample_channel_type))
                : std::nullopt)) {
            continue;
        }
        auto const expected_member = port.node_bundle_port_ordinal.has_value()
            ? std::optional<int>(static_cast<int>(*port.node_bundle_port_ordinal))
            : std::nullopt;
        if (tracked.metadata.int_value(metadata_node_bundle_port_ordinal) != expected_member) {
            continue;
        }
        return tracked.lane;
    }
    return LaneId{};
}

void GraphInputLanes::schedule_instances_for_output_locked(
    std::string_view virtual_node_id,
    std::optional<size_t> member_ordinal,
    size_t output_ordinal,
    PortKind port_kind)
{
    std::unordered_set<std::string> instance_ids;
    for (auto const &port : desired_output_ports) {
        if (port.port.port_kind != port_kind) {
            continue;
        }
        if (port.port.virtual_node_id != virtual_node_id) {
            continue;
        }
        if (port.port.port_ordinal != output_ordinal) {
            continue;
        }
        if (member_ordinal.has_value() && port.port.node_bundle_port_ordinal != member_ordinal) {
            continue;
        }
        instance_ids.insert(port.instance_id);
    }
    for (auto const& instance_id : instance_ids)
        schedule_runtime_binding_sync_locked(instance_id);
}

GraphInputLaneBindings GraphInputLanes::reconcile_ports_locked(TimelineLaneBatchUpdate *batch)
{
    GraphInputLaneBindings result;
    std::unordered_map<std::string, ExistingTrackedLane> virtual_sample_knobs_by_key;
    std::unordered_map<std::string, ExistingTrackedLane> virtual_event_inputs_by_key;
    std::unordered_map<std::string, ExistingTrackedLane> sample_inputs_by_key;
    std::unordered_map<std::string, ExistingTrackedLane> event_inputs_by_key;
    std::unordered_set<std::uint64_t> used_lane_ids;

    for (auto const &tracked : tracked_lanes) {
        lane_ids.observe(tracked.lane);
        if (!tracked.metadata.has_unit(metadata_graph_input)
            || tracked.metadata.has_unit(metadata_public)) {
            continue;
        }
        if (tracked.metadata.has_unit(metadata_knob)
            && tracked.metadata.has_unit(metadata_virtual)
            && tracked.metadata.has_unit(metadata_sample)) {
            virtual_sample_knobs_by_key.emplace(
                existing_identity_key(tracked.metadata, "virtual-knob"),
                tracked);
        } else if (tracked.metadata.has_unit(metadata_input)
            && tracked.metadata.has_unit(metadata_virtual)
            && tracked.metadata.has_unit(metadata_event)) {
            virtual_event_inputs_by_key.emplace(
                existing_identity_key(tracked.metadata, "virtual-event-input"),
                tracked);
        } else if (tracked.metadata.has_unit(metadata_input)
            && tracked.metadata.has_unit(metadata_sample)) {
            sample_inputs_by_key.emplace(
                existing_identity_key(tracked.metadata, "sample-input"),
                tracked);
        } else if (tracked.metadata.has_unit(metadata_input)
            && tracked.metadata.has_unit(metadata_event)) {
            event_inputs_by_key.emplace(
                existing_identity_key(tracked.metadata, "event-input"),
                tracked);
        }
    }

    std::unordered_map<std::string, LaneId> virtual_sample_knob_lanes;
    for (auto const &port : desired_ports) {
        if (port.port.port_kind != PortKind::sample || port.port.node_bundle_port_ordinal.has_value()) {
            continue;
        }
        auto const state_key = graph_input_port_key(port.port);
        auto const state = virtual_sample_knob_states_by_key.find(state_key);
        if (state == virtual_sample_knob_states_by_key.end()
            || state->second != VirtualSampleKnobState::timeline_lane) {
            continue;
        }

        auto const key = virtual_knob_key(port);
        LaneId lane {};
        if (auto const it = virtual_sample_knobs_by_key.find(key);
            it != virtual_sample_knobs_by_key.end()) {
            lane = it->second.lane;
        } else {
            lane = stable_lane_id_for_key(key);
            if (batch != nullptr) {
                auto metadata = graph_input_metadata(
                    port,
                    true,
                    true,
                    false,
                    true,
                    false);
                auto const current_value =
                    live_input_value_or_locked(port.port.virtual_node_id, port.port.port_ordinal, Sample{0.0f});
                auto external_id = lane_external_id_or_new(
                    virtual_sample_knob_lane_ids_by_key,
                    virtual_sample_knobs_by_key,
                    key);
                batch->upserts.push_back(TimelineLaneUpsert{
                    .lane = lane,
                    .external_id = external_id,
                    .make_node = [current_value, name = port.port.port_name] {
                        return TypeErasedLaneNode(KnobLaneNode{
                            .value = current_value,
                            .name = name.empty() ? "graph input" : name,
                        });
                    },
                    .sample_channel_type = port.port.sample_channel_type,
                    .metadata = std::move(metadata),
                });
            }
        }
        used_lane_ids.insert(lane.value);
        virtual_sample_knob_lanes.emplace(key, lane);
        result.virtual_sample_knobs.push_back(GraphInputLaneBinding{
            .port = port.port,
            .knob_lane = lane,
        });
    }

    for (auto const &port : desired_ports) {
        if (port.port.port_kind != PortKind::sample) {
            continue;
        }
        if (!port.port.node_bundle_port_ordinal.has_value()
            && has_node_bundle_descriptor_for_port(desired_ports, port)) {
            continue;
        }

        auto const virtual_key = virtual_knob_key(DesiredGraphPort{
            .instance_id = port.instance_id,
            .module_instance_id = port.module_instance_id,
            .port = GraphInputPortDescriptor{
                .virtual_node_id = port.port.virtual_node_id,
                .node_bundle_port_ordinal = std::nullopt,
                .port_kind = port.port.port_kind,
                .port_ordinal = port.port.port_ordinal,
                .port_name = port.port.port_name,
                .port_type = port.port.port_type,
                .sample_channel_type = port.port.sample_channel_type,
            },
        });
        std::optional<LaneId> virtual_knob;
        if (auto const it = virtual_sample_knob_lanes.find(virtual_key);
            it != virtual_sample_knob_lanes.end()) {
            virtual_knob = it->second;
        }

        NodeBundleSampleInputState state =
            virtual_knob.has_value()
                ? NodeBundleSampleInputState::virtual_follow
                : NodeBundleSampleInputState::disconnected;
        if (!port.port.node_bundle_port_ordinal.has_value()) {
            state = NodeBundleSampleInputState::virtual_follow;
        }
        if (auto const it =
                node_bundle_sample_input_states_by_key.find(graph_input_port_key(port.port));
            it != node_bundle_sample_input_states_by_key.end()) {
            state = it->second;
        }
        if (state != NodeBundleSampleInputState::timeline_lane) {
            continue;
        }

        auto const sample_key = sample_input_key(port);
        LaneId graph_input_lane {};
        if (auto const it = sample_inputs_by_key.find(sample_key);
            it != sample_inputs_by_key.end()) {
            graph_input_lane = it->second.lane;
        } else {
            graph_input_lane = stable_lane_id_for_key(sample_key);
            if (batch != nullptr) {
                auto metadata = graph_input_metadata(
                    port,
                    false,
                    false,
                    true,
                    true,
                    false);
                auto const default_key = sample_default_value_key(port.instance_id, port.port);
                auto const default_value =
                    sample_input_default_values.contains(default_key)
                        ? sample_input_default_values.at(default_key)
                        : Sample{0.0f};
                auto external_id = lane_external_id_or_new(
                    node_bundle_sample_input_lane_ids_by_key,
                    sample_inputs_by_key,
                    sample_key);
                batch->upserts.push_back(TimelineLaneUpsert{
                    .lane = graph_input_lane,
                    .external_id = external_id,
                    .make_node = [default_value] {
                        return make_sample_input_node(default_value);
                    },
                    .sample_channel_type = port.port.sample_channel_type,
                    .metadata = std::move(metadata),
                });
            }
        }
        used_lane_ids.insert(graph_input_lane.value);

        result.sample_inputs.push_back(GraphInputLaneBinding{
            .port = port.port,
            .graph_input_lane = graph_input_lane,
            .virtual_knob_lane = virtual_knob,
        });
    }

    auto ensure_virtual_event_lane = [&](DesiredGraphPort const &port) {
        auto virtual_port = port;
        virtual_port.port.node_bundle_port_ordinal = std::nullopt;
        auto const key = virtual_event_input_key(virtual_port);
        if (auto const it = virtual_event_inputs_by_key.find(key);
            it != virtual_event_inputs_by_key.end()) {
            used_lane_ids.insert(it->second.lane.value);
            return it->second.lane;
        }

        auto const lane = stable_lane_id_for_key(key);
        auto const external_id = lane_external_id_or_new(
            virtual_event_input_lane_ids_by_key,
            virtual_event_inputs_by_key,
            key);
        if (batch != nullptr) {
            auto metadata = graph_input_metadata(
                virtual_port,
                false,
                true,
                false,
                false,
                true);
            batch->upserts.push_back(TimelineLaneUpsert{
                .lane = lane,
                .external_id = external_id,
                .make_node = [] {
                    return TypeErasedLaneNode(GraphEventInputLaneNode{});
                },
                .metadata = std::move(metadata),
            });
        }
        virtual_event_inputs_by_key.emplace(
            key,
            ExistingTrackedLane{
                .lane = lane,
                .external_id = external_id,
                .metadata = graph_input_metadata(
                    virtual_port,
                    false,
                    true,
                    false,
                    false,
                    true),
            });
        used_lane_ids.insert(lane.value);
        return lane;
    };

    for (auto const &port : desired_ports) {
        if (port.port.port_kind != PortKind::event) {
            continue;
        }
        if (!port.port.node_bundle_port_ordinal.has_value()
            && has_node_bundle_descriptor_for_port(desired_ports, port)) {
            continue;
        }

        NodeBundleEventInputState state =
            port.authored_connected
                ? NodeBundleEventInputState::disconnected
                : NodeBundleEventInputState::virtual_follow;
        if (auto const it =
                node_bundle_event_input_states_by_key.find(graph_input_port_key(port.port));
            it != node_bundle_event_input_states_by_key.end()) {
            state = it->second;
        }

        LaneId graph_input_lane {};
        if (state == NodeBundleEventInputState::virtual_follow) {
            graph_input_lane = ensure_virtual_event_lane(port);
        } else if (state == NodeBundleEventInputState::timeline_lane) {
            auto const key = event_input_key(port);
            if (auto const it = event_inputs_by_key.find(key);
                it != event_inputs_by_key.end()) {
                graph_input_lane = it->second.lane;
            } else {
                graph_input_lane = stable_lane_id_for_key(key);
                if (batch != nullptr) {
                    auto metadata = graph_input_metadata(
                        port,
                        false,
                        false,
                        true,
                        false,
                        true);
                    auto external_id = lane_external_id_or_new(
                        node_bundle_event_input_lane_ids_by_key,
                        event_inputs_by_key,
                        key);
                    batch->upserts.push_back(TimelineLaneUpsert{
                        .lane = graph_input_lane,
                        .external_id = external_id,
                        .make_node = [] {
                            return TypeErasedLaneNode(GraphEventInputLaneNode{});
                        },
                        .metadata = std::move(metadata),
                    });
                }
            }
            used_lane_ids.insert(graph_input_lane.value);
        } else {
            continue;
        }

        result.event_inputs.push_back(GraphInputLaneBinding{
            .port = port.port,
            .graph_input_lane = graph_input_lane,
        });
    }

    if (batch != nullptr) {
        for (auto const &tracked : tracked_lanes) {
            if (!tracked.metadata.has_unit(metadata_graph_input)
                || tracked.metadata.has_unit(metadata_public)) {
                continue;
            }
            if (!used_lane_ids.contains(tracked.lane.value)) {
                batch->removals.push_back(tracked.lane);
            }
        }
    }

    return result;
}

GraphInputLaneBindings GraphInputLanes::sample_input_bindings(
    std::string const &node_id,
    std::optional<size_t> member_ordinal,
    size_t input_ordinal,
    ChannelTypeId channel_type)
{
    std::vector<GraphInputPortDescriptor> ports{
        sample_input_descriptor(node_id, std::nullopt, input_ordinal, channel_type),
    };
    if (member_ordinal.has_value()) {
        ports.push_back(sample_input_descriptor(node_id, member_ordinal, input_ordinal, channel_type));
    }
    return query_graph_input_lane_bindings(ProjectGraphInputLaneBindingsRequest{
        .ports = std::move(ports),
    });
}

std::optional<LaneMetadata> GraphInputLanes::tracked_lane_metadata_locked(LaneId lane) const
{
    for (auto const &tracked : tracked_lanes) {
        if (tracked.lane == lane) {
            return tracked.metadata;
        }
    }
    return std::nullopt;
}

void GraphInputLanes::apply_tracked_batch_locked(TimelineLaneBatchUpdate const &batch)
{
    for (auto const lane : batch.removals) {
        for (auto it = tracked_lanes.begin(); it != tracked_lanes.end();) {
            if (it->lane == lane) {
                it = tracked_lanes.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto const &upsert : batch.upserts) {
        bool replaced = false;
        for (auto &tracked : tracked_lanes) {
            if (tracked.lane == upsert.lane) {
                tracked.external_id = upsert.external_id;
                tracked.metadata = upsert.metadata;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            tracked_lanes.push_back(ExistingTrackedLane{
                .lane = upsert.lane,
                .external_id = upsert.external_id,
                .metadata = upsert.metadata,
            });
        }
    }
}

void GraphInputLanes::queue_timeline_batch_locked(TimelineLaneBatchUpdate const &batch)
{
    if (!batch_has_changes(batch)) {
        return;
    }
    auto versioned_batch = batch;
    versioned_batch.version_index = current_update_version_index_;
    apply_tracked_batch_locked(versioned_batch);
    pending_timeline_batches.push_back(std::move(versioned_batch));
}

std::vector<TimelineLaneBatchUpdate> GraphInputLanes::take_pending_timeline_batches_locked()
{
    return std::exchange(pending_timeline_batches, {});
}

void GraphInputLanes::apply_timeline_batch(TimelineLaneBatchUpdate const &batch)
{
    GraphInputLanesAckBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_timeline_batch_requested_event,
        batch,
        builder);
    builder.build();
}

void GraphInputLanes::publish_sample_output_block(
    LaneId lane,
    BorrowedSampleBlock const &block)
{
    output_blocks_.publish_sample(lane, block);
}

void GraphInputLanes::publish_event_output_block(
    LaneId lane,
    std::span<TimedEvent const> events)
{
    output_blocks_.publish_event(lane, events);
}

BorrowedSampleBlock GraphInputLanes::sample_output_block(LaneId lane) const
{
    return output_blocks_.sample(lane);
}

std::span<TimedEvent const> GraphInputLanes::event_output_block(LaneId lane) const
{
    return output_blocks_.event(lane);
}


} // namespace iv
