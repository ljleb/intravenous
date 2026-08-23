#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes/details.h>

namespace iv {
using namespace graph_input_lanes_details;

void GraphInputLanes::set_sample_input_value(
    ProjectSetSampleInputValueRequest const &request)
{
    std::optional<LaneId> knob_lane;
    {
        std::scoped_lock lock(mutex);
        if (request.member_ordinal.has_value()) {
            auto const sample_channel_type = resolve_sample_channel_type(
                desired_ports,
                request.node_id,
                request.member_ordinal,
                request.input_ordinal).value_or(ChannelTypeId::mono);
            auto const key = node_bundle_key(request.node_id, *request.member_ordinal);
            auto const port_key = graph_input_port_key(sample_input_descriptor(
                request.node_id,
                request.member_ordinal,
                request.input_ordinal,
                sample_channel_type));
            node_bundle_live_input_overrides.insert(
                node_bundle_override_key(request.node_id, *request.member_ordinal, request.input_ordinal));
            node_bundle_sample_input_states_by_key[port_key] =
                NodeBundleSampleInputState::overridden;
            ensure_live_input_value_locked(key, request.input_ordinal)
                .store(request.value.value, std::memory_order_relaxed);
            auto &slots = ensure_live_input_slots_locked(key, request.input_ordinal);
            for (auto *slot : slots) {
                if (slot) {
                    *slot = request.value;
                }
            }
        } else {
            auto const sample_channel_type = resolve_sample_channel_type(
                desired_ports,
                request.node_id,
                std::nullopt,
                request.input_ordinal).value_or(ChannelTypeId::mono);
            auto const port_key = graph_input_port_key(sample_input_descriptor(
                request.node_id,
                std::nullopt,
                request.input_ordinal,
                sample_channel_type));
            auto const knob_state = virtual_sample_knob_states_by_key.find(port_key);
            bool const timeline_knob = knob_state != virtual_sample_knob_states_by_key.end()
                && knob_state->second == VirtualSampleKnobState::timeline_lane;
            if (!timeline_knob) {
                virtual_sample_knob_states_by_key[port_key] =
                    VirtualSampleKnobState::overridden;
            }
            ensure_live_input_value_locked(request.node_id, request.input_ordinal)
                .store(request.value.value, std::memory_order_relaxed);
            if (timeline_knob) {
                knob_lane = stable_lane_id_for_key(virtual_knob_key(DesiredGraphPort{
                    .port = sample_input_descriptor(
                        request.node_id,
                        std::nullopt,
                        request.input_ordinal,
                        sample_channel_type),
                }));
            }
            if (auto it = live_inputs.find(std::string(request.node_id));
                it != live_inputs.end() && it->second.size() > request.input_ordinal) {
                for (auto *slot : it->second[request.input_ordinal]) {
                    if (slot) {
                        *slot = request.value;
                    }
                }
            }
            std::string const prefix = node_bundle_key_prefix(request.node_id);
            for (auto &[key, slots_by_input] : live_inputs) {
                if (!key.starts_with(prefix) || slots_by_input.size() <= request.input_ordinal) {
                    continue;
                }
                auto const input_override_key = key + "\x1finput:" + std::to_string(request.input_ordinal);
                if (node_bundle_live_input_overrides.contains(input_override_key)) {
                    continue;
                }
                for (auto *slot : slots_by_input[request.input_ordinal]) {
                    if (slot) {
                        *slot = request.value;
                    }
                }
                ensure_live_input_value_locked(key, request.input_ordinal)
                    .store(request.value.value, std::memory_order_relaxed);
            }
        }
        if (request.member_ordinal.has_value()) {
            schedule_instances_for_input_locked(
                request.node_id, request.member_ordinal, request.input_ordinal);
        } else {
            schedule_instances_for_virtual_sample_input_locked(
                request.node_id, request.input_ordinal);
        }
    }
    if (knob_lane.has_value()) {
        IV_INVOKE_SINGLETON_EVENT(
            iv_runtime_graph_input_lanes_knob_value_updated_event,
            *knob_lane,
            request.value);
    }
}

void GraphInputLanes::set_sample_input_state(
    ProjectSetSampleInputStateRequest const &request)
{
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        if (request.member_ordinal.has_value()) {
            auto const sample_key = sample_input_key(DesiredGraphPort{
                .instance_id = {},
                .module_instance_id = 0,
                .port = sample_input_descriptor(
                    request.node_id,
                    request.member_ordinal,
                    request.input_ordinal,
                    resolve_sample_channel_type(
                        desired_ports,
                        request.node_id,
                        request.member_ordinal,
                        request.input_ordinal).value_or(ChannelTypeId::mono)),
            });
            auto const sample_channel_type = resolve_sample_channel_type(
                desired_ports,
                request.node_id,
                request.member_ordinal,
                request.input_ordinal).value_or(ChannelTypeId::mono);
            auto const port_key = graph_input_port_key(sample_input_descriptor(
                request.node_id,
                request.member_ordinal,
                request.input_ordinal,
                sample_channel_type));
            auto const override_key = node_bundle_override_key(
                request.node_id,
                *request.member_ordinal,
                request.input_ordinal);

            switch (request.state) {
            case ProjectSampleInputState::default_:
                node_bundle_live_input_overrides.erase(override_key);
                node_bundle_sample_input_states_by_key.erase(port_key);
                node_bundle_sample_input_lane_ids_by_key.erase(sample_key);
                break;
            case ProjectSampleInputState::overridden:
                node_bundle_live_input_overrides.insert(override_key);
                node_bundle_sample_input_states_by_key[port_key] =
                    NodeBundleSampleInputState::overridden;
                node_bundle_sample_input_lane_ids_by_key.erase(sample_key);
                break;
            case ProjectSampleInputState::virtual_follow:
                node_bundle_live_input_overrides.erase(override_key);
                node_bundle_sample_input_states_by_key[port_key] =
                    NodeBundleSampleInputState::virtual_follow;
                node_bundle_sample_input_lane_ids_by_key.erase(sample_key);
                break;
            case ProjectSampleInputState::timeline_lane:
                node_bundle_live_input_overrides.erase(override_key);
                node_bundle_sample_input_states_by_key[port_key] =
                    NodeBundleSampleInputState::timeline_lane;
                if (request.lane_id.has_value()) {
                    node_bundle_sample_input_lane_ids_by_key[sample_key] = *request.lane_id;
                } else {
                    node_bundle_sample_input_lane_ids_by_key.erase(sample_key);
                }
                break;
            case ProjectSampleInputState::disconnected:
                node_bundle_live_input_overrides.erase(override_key);
                node_bundle_sample_input_states_by_key[port_key] =
                    NodeBundleSampleInputState::disconnected;
                node_bundle_sample_input_lane_ids_by_key.erase(sample_key);
                break;
            }

            schedule_instances_for_input_locked(
                request.node_id,
                request.member_ordinal,
                request.input_ordinal);
        } else {
            auto const virtual_key = virtual_knob_key(DesiredGraphPort{
                .instance_id = {},
                .module_instance_id = 0,
                .port = sample_input_descriptor(
                    request.node_id,
                    std::nullopt,
                    request.input_ordinal,
                    resolve_sample_channel_type(
                        desired_ports,
                        request.node_id,
                        std::nullopt,
                        request.input_ordinal).value_or(ChannelTypeId::mono)),
            });
            auto const sample_channel_type = resolve_sample_channel_type(
                desired_ports,
                request.node_id,
                std::nullopt,
                request.input_ordinal).value_or(ChannelTypeId::mono);
            auto const port_key = graph_input_port_key(sample_input_descriptor(
                request.node_id,
                std::nullopt,
                request.input_ordinal,
                sample_channel_type));
            switch (request.state) {
            case ProjectSampleInputState::default_:
            case ProjectSampleInputState::overridden:
                virtual_sample_knob_states_by_key[port_key] =
                    VirtualSampleKnobState::overridden;
                virtual_sample_knob_lane_ids_by_key.erase(virtual_key);
                break;
            case ProjectSampleInputState::timeline_lane:
                virtual_sample_knob_states_by_key[port_key] =
                    VirtualSampleKnobState::timeline_lane;
                if (request.lane_id.has_value()) {
                    virtual_sample_knob_lane_ids_by_key[virtual_key] = *request.lane_id;
                } else {
                    virtual_sample_knob_lane_ids_by_key.erase(virtual_key);
                }
                break;
            case ProjectSampleInputState::virtual_follow:
            case ProjectSampleInputState::disconnected:
                throw std::runtime_error(
                    "virtual sample input state only supports overridden or timeline_lane");
            }
            schedule_instances_for_virtual_sample_input_locked(
                request.node_id,
                request.input_ordinal);
        }
        (void)reconcile_ports_locked(&batch);
        queue_timeline_batch_locked(batch);
    }
}

void GraphInputLanes::set_public_sample_input_state(
    ProjectSetPublicSampleInputStateRequest const &request)
{
    if (request.instance_id.empty() || request.source_identity.empty()) {
        throw std::runtime_error("public sample input state requires instance and source identities");
    }

    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        auto const state_key = public_sample_input_state_key(
            request.instance_id, request.source_identity, request.member_ordinal);

        if (!request.member_ordinal.has_value()
            && request.state == ProjectSampleInputState::default_) {
            public_sample_input_states_by_key.erase(state_key);
            public_sample_input_lane_ids_by_key.erase(state_key);
            schedule_runtime_binding_sync_locked(request.instance_id);
            reconcile_public_ports_locked(&batch);
            queue_timeline_batch_locked(batch);
            return;
        } else if (!request.member_ordinal.has_value()) {
            if (request.state != ProjectSampleInputState::overridden
                && request.state != ProjectSampleInputState::timeline_lane
                && request.state != ProjectSampleInputState::disconnected) {
                throw std::runtime_error(
                    "virtual public sample input state only supports overridden, timeline_lane, or disconnected");
            }
        } else if (request.state == ProjectSampleInputState::default_) {
            public_sample_input_states_by_key.erase(state_key);
            public_sample_input_lane_ids_by_key.erase(state_key);
            schedule_runtime_binding_sync_locked(request.instance_id);
            reconcile_public_ports_locked(&batch);
            queue_timeline_batch_locked(batch);
            return;
        }

        public_sample_input_states_by_key[state_key] = request.state;
        if (request.state == ProjectSampleInputState::timeline_lane
            && request.lane_id.has_value()) {
            // Lanes are keyed by public-port identity.  The state key has the
            // same virtual/member partition and is deliberately used here so
            // callers can retain a chosen timeline external id.
            public_sample_input_lane_ids_by_key[state_key] = *request.lane_id;
        } else {
            public_sample_input_lane_ids_by_key.erase(state_key);
        }
        schedule_runtime_binding_sync_locked(request.instance_id);
        reconcile_public_ports_locked(&batch);
        queue_timeline_batch_locked(batch);
    }
}

void GraphInputLanes::set_public_sample_input_value(
    std::string const &instance_id,
    std::string const &source_identity,
    Sample value)
{
    if (instance_id.empty() || source_identity.empty()) {
        throw std::runtime_error("public sample input value requires instance and source identities");
    }
    std::vector<LaneId> live_value_lanes;
    {
        std::scoped_lock lock(mutex);
        auto const key = public_sample_input_state_key(instance_id, source_identity, std::nullopt);
        ensure_public_sample_input_value_locked(instance_id, source_identity, Sample{0.0f})
            .store(value.value, std::memory_order_relaxed);
        auto const state = public_sample_input_states_by_key.find(key);
        bool const timeline_input = state == public_sample_input_states_by_key.end()
            || state->second == ProjectSampleInputState::timeline_lane;
        if (!timeline_input) {
            public_sample_input_states_by_key[key] = ProjectSampleInputState::overridden;
            public_sample_input_lane_ids_by_key.erase(key);
        } else {
            for (auto const &port : desired_public_input_ports) {
                if (!port.input || port.port_kind != PortKind::sample
                    || port.instance_id != instance_id
                    || port.source_identity != source_identity) {
                    continue;
                }
                auto const member_key = public_sample_input_state_key(
                    instance_id, source_identity, port.port_ordinal);
                if (auto const member = public_sample_input_states_by_key.find(member_key);
                    member != public_sample_input_states_by_key.end()
                    && member->second == ProjectSampleInputState::timeline_lane) {
                    continue;
                }
                live_value_lanes.push_back(public_graph_port_lane_for(port));
            }
        }
        schedule_runtime_binding_sync_locked(instance_id);
    }
    for (auto const lane : live_value_lanes) {
        IV_INVOKE_SINGLETON_EVENT(
            iv_runtime_graph_input_lanes_knob_value_updated_event,
            lane,
            value);
    }
}

void GraphInputLanes::set_public_event_input_state(
    std::string const &instance_id, std::string const &source_identity,
    std::optional<size_t> member_ordinal, ProjectEventInputState state,
    std::optional<InternedString> lane_id)
{
    if (instance_id.empty() || source_identity.empty()) throw std::runtime_error("public event input state requires identities");
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        auto const key = public_sample_input_state_key(instance_id, source_identity, member_ordinal);
        if (!member_ordinal.has_value()
            && state != ProjectEventInputState::default_
            && state != ProjectEventInputState::timeline_lane
            && state != ProjectEventInputState::disconnected) {
            throw std::runtime_error(
                "virtual public event input state only supports timeline_lane or disconnected");
        }
        if (state == ProjectEventInputState::default_) {
            public_event_input_states_by_key.erase(key);
            public_event_input_lane_ids_by_key.erase(key);
        } else {
            public_event_input_states_by_key[key] = state;
            if (state == ProjectEventInputState::timeline_lane && lane_id.has_value()) {
                public_event_input_lane_ids_by_key[key] = *lane_id;
            } else {
                public_event_input_lane_ids_by_key.erase(key);
            }
        }
        schedule_runtime_binding_sync_locked(instance_id);
        reconcile_public_ports_locked(&batch);
        queue_timeline_batch_locked(batch);
    }
}

std::vector<PublicSampleInputInfo> GraphInputLanes::public_sample_inputs() const
{
    std::scoped_lock lock(mutex);
    std::unordered_map<std::string, size_t> indices;
    std::vector<PublicSampleInputInfo> result;
    for (auto const &port : desired_public_input_ports) {
        if (port.port_kind != PortKind::sample || port.source_identity.empty()) {
            continue;
        }
        auto const key = public_sample_input_state_key(
            port.instance_id, port.source_identity, std::nullopt);
        auto [it, inserted] = indices.try_emplace(key, result.size());
        if (inserted) {
            auto const state_it = public_sample_input_states_by_key.find(key);
            auto const state = state_it == public_sample_input_states_by_key.end()
                ? ProjectSampleInputState::timeline_lane
                : state_it->second;
            auto const value_it = public_sample_input_values.find(key);
            auto const current_value = value_it == public_sample_input_values.end()
                ? port.default_value
                : Sample{value_it->second->load(std::memory_order_relaxed)};
            result.push_back(PublicSampleInputInfo{
                .instance_id = port.instance_id,
                .source_identity = port.source_identity,
                .source_infos = port.source_infos,
                .name = port.port_name,
                .default_value = port.default_value,
                .min = port.min,
                .max = port.max,
                .current_value = current_value,
                .virtual_state = state == ProjectSampleInputState::overridden ? "overridden"
                    : state == ProjectSampleInputState::timeline_lane ? "timelineLane"
                    : state == ProjectSampleInputState::disconnected ? "disconnected"
                    : "default",
                .graph_connected = port.graph_connected,
            });
        }
        result[it->second].member_ordinals.push_back(port.port_ordinal);
        result[it->second].member_graph_connected.push_back(port.graph_connected);
        auto const member_key = public_sample_input_state_key(
            port.instance_id, port.source_identity, port.port_ordinal);
        auto const member_state_it = public_sample_input_states_by_key.find(member_key);
        auto const member_state = member_state_it == public_sample_input_states_by_key.end()
            ? ProjectSampleInputState::virtual_follow
            : member_state_it->second;
        result[it->second].member_states.push_back(
            member_state == ProjectSampleInputState::timeline_lane ? "timelineLane"
                : member_state == ProjectSampleInputState::disconnected ? "disconnected"
                    : member_state == ProjectSampleInputState::overridden ? "overridden"
                        : "virtualFollow");
    }
    return result;
}

std::vector<PublicEventInputInfo> GraphInputLanes::public_event_inputs() const
{
    std::scoped_lock lock(mutex);
    std::unordered_map<std::string, size_t> indices;
    std::vector<PublicEventInputInfo> result;
    for (auto const &port : desired_public_input_ports) {
        if (port.port_kind != PortKind::event || port.source_identity.empty() || !port.event_type.has_value()) continue;
        auto const key = public_sample_input_state_key(port.instance_id, port.source_identity, std::nullopt);
        auto [it, inserted] = indices.try_emplace(key, result.size());
        if (inserted) {
            auto const state_it = public_event_input_states_by_key.find(key);
            auto const state = state_it == public_event_input_states_by_key.end()
                ? ProjectEventInputState::timeline_lane : state_it->second;
            result.push_back(PublicEventInputInfo{
                .instance_id = port.instance_id,
                .source_identity = port.source_identity,
                .source_infos = port.source_infos,
                .name = port.port_name,
                .type = *port.event_type,
                .virtual_state = state == ProjectEventInputState::disconnected ? "disconnected" : "timelineLane",
                .graph_connected = port.graph_connected,
            });
        }
        auto const member_ordinal = port.node_bundle_port_ordinal.value_or(port.port_ordinal);
        auto const member_key = public_sample_input_state_key(port.instance_id, port.source_identity, member_ordinal);
        auto const member_it = public_event_input_states_by_key.find(member_key);
        auto const member_state = member_it == public_event_input_states_by_key.end()
            ? ProjectEventInputState::virtual_follow : member_it->second;
        result[it->second].member_ordinals.push_back(port.port_ordinal);
        result[it->second].member_graph_connected.push_back(port.graph_connected);
        result[it->second].member_states.push_back(
            member_state == ProjectEventInputState::timeline_lane ? "timelineLane"
                : member_state == ProjectEventInputState::disconnected ? "disconnected"
                    : "virtualFollow");
    }
    return result;
}

std::vector<PublicSampleOutputInfo> GraphInputLanes::public_sample_outputs() const
{
    std::scoped_lock lock(mutex);
    std::vector<PublicSampleOutputInfo> result;
    std::unordered_map<std::string, size_t> index;
    for (auto const& port : desired_public_output_ports) {
        if (port.port_kind != PortKind::sample || port.source_identity.empty()) continue;
        auto const virtual_key = public_sample_input_state_key(port.instance_id, port.source_identity, std::nullopt);
        auto const member_ordinal = port.node_bundle_port_ordinal.value_or(port.port_ordinal);
        auto const member_key = public_sample_input_state_key(port.instance_id, port.source_identity, member_ordinal);
        auto const virtual_it = public_sample_output_states_by_key.find(virtual_key);
        auto const member_it = public_sample_output_states_by_key.find(member_key);
        auto const virtual_connected = virtual_it == public_sample_output_states_by_key.end()
            || virtual_it->second == ProjectSampleOutputState::timeline_lane;
        auto const member_state = member_it == public_sample_output_states_by_key.end()
            ? ProjectSampleOutputState::virtual_port : member_it->second;
        auto const key = port.instance_id + "\x1f" + port.source_identity;
        auto [it, inserted] = index.emplace(key, result.size());
        if (inserted) result.push_back(PublicSampleOutputInfo{
            .instance_id = port.instance_id, .source_identity = port.source_identity,
            .source_infos = port.source_infos, .name = port.port_name,
            .virtual_state = virtual_connected ? "timelineLane" : "disconnected",
            .graph_connected = virtual_connected,
        });
        auto& output = result[it->second];
        output.member_ordinals.push_back(member_ordinal);
        output.member_graph_connected.push_back(member_state == ProjectSampleOutputState::timeline_lane
            || (member_state == ProjectSampleOutputState::virtual_port && virtual_connected));
        output.member_states.push_back(member_state == ProjectSampleOutputState::timeline_lane ? "timelineLane"
            : member_state == ProjectSampleOutputState::disconnected ? "disconnected" : "virtualFollow");
    }
    return result;
}

std::vector<PublicEventOutputInfo> GraphInputLanes::public_event_outputs() const
{
    std::scoped_lock lock(mutex);
    std::vector<PublicEventOutputInfo> result;
    std::unordered_map<std::string, size_t> index;
    for (auto const& port : desired_public_output_ports) {
        if (port.port_kind != PortKind::event || port.source_identity.empty() || !port.event_type) continue;
        auto const virtual_key = public_sample_input_state_key(port.instance_id, port.source_identity, std::nullopt);
        auto const member_ordinal = port.node_bundle_port_ordinal.value_or(port.port_ordinal);
        auto const member_key = public_sample_input_state_key(port.instance_id, port.source_identity, member_ordinal);
        auto const virtual_it = public_event_output_states_by_key.find(virtual_key);
        auto const member_it = public_event_output_states_by_key.find(member_key);
        auto const virtual_connected = virtual_it == public_event_output_states_by_key.end()
            || virtual_it->second == ProjectEventOutputState::timeline_lane;
        auto const member_state = member_it == public_event_output_states_by_key.end()
            ? ProjectEventOutputState::virtual_port : member_it->second;
        auto const key = port.instance_id + "\x1f" + port.source_identity;
        auto [it, inserted] = index.emplace(key, result.size());
        if (inserted) result.push_back(PublicEventOutputInfo{
            .instance_id = port.instance_id, .source_identity = port.source_identity,
            .source_infos = port.source_infos, .name = port.port_name, .type = *port.event_type,
            .virtual_state = virtual_connected ? "timelineLane" : "disconnected", .graph_connected = virtual_connected,
        });
        auto& output = result[it->second];
        output.member_ordinals.push_back(member_ordinal);
        output.member_graph_connected.push_back(member_state == ProjectEventOutputState::timeline_lane
            || (member_state == ProjectEventOutputState::virtual_port && virtual_connected));
        output.member_states.push_back(member_state == ProjectEventOutputState::timeline_lane ? "timelineLane"
            : member_state == ProjectEventOutputState::disconnected ? "disconnected" : "virtualFollow");
    }
    return result;
}

void GraphInputLanes::set_event_input_state(
    ProjectSetEventInputStateRequest const &request)
{
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        auto const node_bundle_key_value = event_input_key(DesiredGraphPort{
            .instance_id = {},
            .module_instance_id = 0,
            .port = GraphInputPortDescriptor{
                .virtual_node_id = request.node_id,
                .node_bundle_port_ordinal = request.member_ordinal,
                .port_kind = PortKind::event,
                .port_ordinal = request.input_ordinal,
            },
        });
        auto const virtual_key_value = virtual_event_input_key(DesiredGraphPort{
            .instance_id = {},
            .module_instance_id = 0,
            .port = GraphInputPortDescriptor{
                .virtual_node_id = request.node_id,
                .node_bundle_port_ordinal = std::nullopt,
                .port_kind = PortKind::event,
                .port_ordinal = request.input_ordinal,
            },
        });
        auto const port_key = graph_input_port_key(GraphInputPortDescriptor{
            .virtual_node_id = request.node_id,
            .node_bundle_port_ordinal = request.member_ordinal,
            .port_kind = PortKind::event,
            .port_ordinal = request.input_ordinal,
        });

        switch (request.state) {
        case ProjectEventInputState::default_:
            node_bundle_event_input_states_by_key.erase(port_key);
            node_bundle_event_input_lane_ids_by_key.erase(node_bundle_key_value);
            break;
        case ProjectEventInputState::virtual_follow:
            node_bundle_event_input_states_by_key[port_key] =
                NodeBundleEventInputState::virtual_follow;
            node_bundle_event_input_lane_ids_by_key.erase(node_bundle_key_value);
            if (request.lane_id.has_value()) {
                virtual_event_input_lane_ids_by_key[virtual_key_value] = *request.lane_id;
            }
            break;
        case ProjectEventInputState::timeline_lane:
            node_bundle_event_input_states_by_key[port_key] =
                NodeBundleEventInputState::timeline_lane;
            if (request.lane_id.has_value()) {
                node_bundle_event_input_lane_ids_by_key[node_bundle_key_value] = *request.lane_id;
            } else {
                node_bundle_event_input_lane_ids_by_key.erase(node_bundle_key_value);
            }
            break;
        case ProjectEventInputState::disconnected:
            node_bundle_event_input_states_by_key[port_key] =
                NodeBundleEventInputState::disconnected;
            node_bundle_event_input_lane_ids_by_key.erase(node_bundle_key_value);
            break;
        }

        bool has_virtual_follow_remaining = false;
        for (auto const &port : desired_ports) {
            if (port.port.port_kind != PortKind::event) {
                continue;
            }
            if (port.port.virtual_node_id != request.node_id) {
                continue;
            }
            if (port.port.port_ordinal != request.input_ordinal) {
                continue;
            }
            auto const candidate_port_key = graph_input_port_key(GraphInputPortDescriptor{
                .virtual_node_id = port.port.virtual_node_id,
                .node_bundle_port_ordinal = port.port.node_bundle_port_ordinal,
                .port_kind = PortKind::event,
                .port_ordinal = port.port.port_ordinal,
            });
            if (auto const it = node_bundle_event_input_states_by_key.find(candidate_port_key);
                it != node_bundle_event_input_states_by_key.end()
                && it->second == NodeBundleEventInputState::virtual_follow) {
                has_virtual_follow_remaining = true;
                break;
            }
        }
        if (!has_virtual_follow_remaining) {
            virtual_event_input_lane_ids_by_key.erase(virtual_key_value);
        }

        schedule_instances_for_input_locked(
            request.node_id,
            request.member_ordinal,
            request.input_ordinal);
        (void)reconcile_ports_locked(&batch);
        queue_timeline_batch_locked(batch);
    }
}

void GraphInputLanes::set_sample_output_state(
    ProjectSetSampleOutputStateRequest const &request)
{
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        if (auto const public_output = parse_public_output_node_id(request.node_id)) {
            auto const key = public_sample_input_state_key(
                public_output->first, public_output->second, request.member_ordinal);
            if (request.member_ordinal.has_value()) {
                if (request.state == ProjectSampleOutputState::virtual_port) {
                    public_sample_output_states_by_key.erase(key);
                } else {
                    public_sample_output_states_by_key[key] = request.state;
                }
            } else {
                if (request.state == ProjectSampleOutputState::timeline_lane) {
                    public_sample_output_states_by_key.erase(key);
                } else if (request.state == ProjectSampleOutputState::disconnected) {
                    public_sample_output_states_by_key[key] = request.state;
                } else {
                    throw std::runtime_error("virtual public sample output only supports connected or disconnected");
                }
            }
            (void)reconcile_ports_locked(&batch);
            schedule_runtime_binding_sync_locked(public_output->first);
            queue_timeline_batch_locked(batch);
            return;
        }
        auto const identity_key = output_identity_key(
            DesiredGraphPort{
                .instance_id = {},
                .module_instance_id = 0,
                .port = GraphInputPortDescriptor{
                    .virtual_node_id = request.node_id,
                    .node_bundle_port_ordinal = request.member_ordinal,
                    .port_kind = PortKind::sample,
                    .port_ordinal = request.output_ordinal,
                    .sample_channel_type = resolve_sample_channel_type(
                        desired_output_ports,
                        request.node_id,
                        request.member_ordinal,
                        request.output_ordinal).value_or(ChannelTypeId::mono),
                },
            },
            request.member_ordinal.has_value() ? "node-bundle-output" : "virtual-output");
        auto const sample_channel_type = resolve_sample_channel_type(
            desired_output_ports,
            request.node_id,
            request.member_ordinal,
            request.output_ordinal).value_or(ChannelTypeId::mono);
        auto const port_key = graph_input_port_key(GraphInputPortDescriptor{
            .virtual_node_id = request.node_id,
            .node_bundle_port_ordinal = request.member_ordinal,
            .port_kind = PortKind::sample,
            .port_ordinal = request.output_ordinal,
            .sample_channel_type = sample_channel_type,
        });

        if (request.member_ordinal.has_value()) {
            switch (request.state) {
            case ProjectSampleOutputState::disconnected:
                node_bundle_output_states_by_key.erase(port_key);
                node_bundle_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectSampleOutputState::virtual_port:
                node_bundle_output_states_by_key[port_key] = NodeBundleOutputState::virtual_port;
                node_bundle_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectSampleOutputState::timeline_lane:
                node_bundle_output_states_by_key[port_key] = NodeBundleOutputState::timeline_lane;
                if (request.lane_id.has_value()) {
                    node_bundle_output_lane_ids_by_key[identity_key] = *request.lane_id;
                } else {
                    node_bundle_output_lane_ids_by_key.erase(identity_key);
                }
                break;
            }
        } else {
            switch (request.state) {
            case ProjectSampleOutputState::disconnected:
                virtual_output_states_by_key.erase(port_key);
                virtual_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectSampleOutputState::timeline_lane:
                virtual_output_states_by_key[port_key] = VirtualOutputState::timeline_lane;
                if (request.lane_id.has_value()) {
                    virtual_output_lane_ids_by_key[identity_key] = *request.lane_id;
                } else {
                    virtual_output_lane_ids_by_key.erase(identity_key);
                }
                break;
            case ProjectSampleOutputState::virtual_port:
                throw std::runtime_error(
                    "virtual sample output state only supports disconnected or timeline_lane");
            }
        }

        schedule_instances_for_output_locked(
            request.node_id,
            request.member_ordinal,
            request.output_ordinal,
            PortKind::sample);
        reconcile_output_ports_locked(&batch);
        queue_timeline_batch_locked(batch);
    }
}

void GraphInputLanes::set_event_output_state(
    ProjectSetEventOutputStateRequest const &request)
{
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        if (auto const public_output = parse_public_output_node_id(request.node_id)) {
            auto const key = public_sample_input_state_key(
                public_output->first, public_output->second, request.member_ordinal);
            if (request.member_ordinal.has_value()) {
                if (request.state == ProjectEventOutputState::virtual_port) {
                    public_event_output_states_by_key.erase(key);
                } else {
                    public_event_output_states_by_key[key] = request.state;
                }
            } else {
                if (request.state == ProjectEventOutputState::timeline_lane) {
                    public_event_output_states_by_key.erase(key);
                } else if (request.state == ProjectEventOutputState::disconnected) {
                    public_event_output_states_by_key[key] = request.state;
                } else {
                    throw std::runtime_error("virtual public event output only supports connected or disconnected");
                }
            }
            (void)reconcile_ports_locked(&batch);
            schedule_runtime_binding_sync_locked(public_output->first);
            queue_timeline_batch_locked(batch);
            return;
        }
        auto const identity_key = output_identity_key(
            DesiredGraphPort{
                .instance_id = {},
                .module_instance_id = 0,
                .port = GraphInputPortDescriptor{
                    .virtual_node_id = request.node_id,
                    .node_bundle_port_ordinal = request.member_ordinal,
                    .port_kind = PortKind::event,
                    .port_ordinal = request.output_ordinal,
                },
            },
            request.member_ordinal.has_value() ? "node-bundle-output" : "virtual-output");
        auto const port_key = graph_input_port_key(GraphInputPortDescriptor{
            .virtual_node_id = request.node_id,
            .node_bundle_port_ordinal = request.member_ordinal,
            .port_kind = PortKind::event,
            .port_ordinal = request.output_ordinal,
        });

        if (request.member_ordinal.has_value()) {
            switch (request.state) {
            case ProjectEventOutputState::disconnected:
                node_bundle_output_states_by_key.erase(port_key);
                node_bundle_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectEventOutputState::virtual_port:
                node_bundle_output_states_by_key[port_key] = NodeBundleOutputState::virtual_port;
                node_bundle_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectEventOutputState::timeline_lane:
                node_bundle_output_states_by_key[port_key] = NodeBundleOutputState::timeline_lane;
                if (request.lane_id.has_value()) {
                    node_bundle_output_lane_ids_by_key[identity_key] = *request.lane_id;
                } else {
                    node_bundle_output_lane_ids_by_key.erase(identity_key);
                }
                break;
            }
        } else {
            switch (request.state) {
            case ProjectEventOutputState::disconnected:
                virtual_output_states_by_key.erase(port_key);
                virtual_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectEventOutputState::timeline_lane:
                virtual_output_states_by_key[port_key] = VirtualOutputState::timeline_lane;
                if (request.lane_id.has_value()) {
                    virtual_output_lane_ids_by_key[identity_key] = *request.lane_id;
                } else {
                    virtual_output_lane_ids_by_key.erase(identity_key);
                }
                break;
            case ProjectEventOutputState::virtual_port:
                throw std::runtime_error(
                    "virtual event output state only supports disconnected or timeline_lane");
            }
        }

        schedule_instances_for_output_locked(
            request.node_id,
            request.member_ordinal,
            request.output_ordinal,
            PortKind::event);
        reconcile_output_ports_locked(&batch);
        queue_timeline_batch_locked(batch);
    }
}

GraphInputLaneBindings GraphInputLanes::graph_input_lane_bindings(
    ProjectGraphInputLaneBindingsRequest const &request)
{
    return query_graph_input_lane_bindings(request);
}

GraphInputLanes::AuthoredStateSnapshot GraphInputLanes::authored_state() const
{
    std::scoped_lock lock(mutex);
    AuthoredStateSnapshot snapshot;
    std::unordered_map<std::string, InternedString> virtual_sample_knob_external_ids_by_key;
    std::unordered_map<std::string, InternedString> sample_input_external_ids_by_key;
    std::unordered_map<std::string, InternedString> virtual_event_input_external_ids_by_key;
    std::unordered_map<std::string, InternedString> event_input_external_ids_by_key;
    std::unordered_map<std::string, InternedString> virtual_output_external_ids_by_key;
    std::unordered_map<std::string, InternedString> node_bundle_output_external_ids_by_key;

    // Public inputs are persisted through the existing graph sample-input
    // commands, using their synthetic source-query node id.  Project loading
    // bypasses socket RPC, so the runtime-project bridge recognizes that id
    // and routes it back to the public-input model.
    std::unordered_set<std::string> persisted_public_virtual_inputs;
    for (auto const &port : desired_public_input_ports) {
        if (port.port_kind != PortKind::sample || port.source_identity.empty()) {
            continue;
        }
        auto const virtual_key = public_sample_input_state_key(
            port.instance_id, port.source_identity, std::nullopt);
        auto const synthetic_node_id = public_sample_input_node_id(
            port.instance_id, port.source_identity);
        if (persisted_public_virtual_inputs.insert(virtual_key).second) {
            auto const state_it = public_sample_input_states_by_key.find(virtual_key);
            auto const state = state_it == public_sample_input_states_by_key.end()
                ? ProjectSampleInputState::timeline_lane
                : state_it->second;
            if (state == ProjectSampleInputState::overridden) {
                auto const value_it = public_sample_input_values.find(virtual_key);
                snapshot.sample_input_values.push_back(ProjectSetSampleInputValueRequest{
                    .node_id = synthetic_node_id,
                    .input_ordinal = 0,
                    .value = value_it == public_sample_input_values.end()
                        ? port.default_value
                        : Sample{value_it->second->load(std::memory_order_relaxed)},
                });
            }
            // Persist the choice as well as an overridden value. Replaying
            // only the value left the default timeline lane active and
            // reconnected the public input after a window reload.
            if (state != ProjectSampleInputState::timeline_lane) {
                snapshot.sample_input_states.push_back(ProjectSetSampleInputStateRequest{
                    .node_id = synthetic_node_id,
                    .input_ordinal = 0,
                    .state = state,
                });
            }
        }

        auto const member_key = public_sample_input_state_key(
            port.instance_id, port.source_identity, port.port_ordinal);
        if (auto const state_it = public_sample_input_states_by_key.find(member_key);
            state_it != public_sample_input_states_by_key.end()) {
            ProjectSampleInputState state = ProjectSampleInputState::virtual_follow;
            switch (state_it->second) {
            case ProjectSampleInputState::default_:
            case ProjectSampleInputState::virtual_follow:
                state = ProjectSampleInputState::virtual_follow;
                break;
            case ProjectSampleInputState::timeline_lane:
                state = ProjectSampleInputState::timeline_lane;
                break;
            case ProjectSampleInputState::disconnected:
                state = ProjectSampleInputState::disconnected;
                break;
            case ProjectSampleInputState::overridden:
                continue;
            }
            snapshot.sample_input_states.push_back(ProjectSetSampleInputStateRequest{
                .node_id = synthetic_node_id,
                .member_ordinal = port.port_ordinal,
                .input_ordinal = 0,
                .state = state,
            });
        }
    }

    std::unordered_set<std::string> persisted_public_virtual_events;
    for (auto const &port : desired_public_input_ports) {
        if (port.port_kind != PortKind::event || port.source_identity.empty()) continue;
        auto const virtual_key = public_sample_input_state_key(port.instance_id, port.source_identity, std::nullopt);
        auto const node_id = public_sample_input_node_id(port.instance_id, port.source_identity);
        if (persisted_public_virtual_events.insert(virtual_key).second) {
            if (auto const it = public_event_input_states_by_key.find(virtual_key);
                it != public_event_input_states_by_key.end()
                && (it->second != ProjectEventInputState::timeline_lane
                    || public_event_input_lane_ids_by_key.contains(virtual_key))) {
                snapshot.event_input_states.push_back(ProjectSetEventInputStateRequest{
                    .node_id = node_id, .input_ordinal = 0, .state = it->second,
                    .lane_id = [&]() -> std::optional<InternedString> {
                        if (auto const lane = public_event_input_lane_ids_by_key.find(virtual_key);
                            lane != public_event_input_lane_ids_by_key.end()) return lane->second;
                        return std::nullopt;
                    }(),
                });
            }
        }
        auto const member_key = public_sample_input_state_key(port.instance_id, port.source_identity, port.port_ordinal);
        if (auto const it = public_event_input_states_by_key.find(member_key);
            it != public_event_input_states_by_key.end()) {
            snapshot.event_input_states.push_back(ProjectSetEventInputStateRequest{
                .node_id = node_id, .member_ordinal = port.port_ordinal,
                .input_ordinal = 0, .state = it->second,
                .lane_id = [&]() -> std::optional<InternedString> {
                    if (auto const lane = public_event_input_lane_ids_by_key.find(member_key);
                        lane != public_event_input_lane_ids_by_key.end()) return lane->second;
                    return std::nullopt;
                }(),
            });
        }
    }

    for (auto const &tracked : tracked_lanes) {
        if (tracked.external_id.empty()) {
            continue;
        }
        if (tracked.metadata.has_unit(metadata_graph_input)) {
            if (tracked.metadata.has_unit(metadata_knob)
                && tracked.metadata.has_unit(metadata_virtual)
                && tracked.metadata.has_unit(metadata_sample)) {
                virtual_sample_knob_external_ids_by_key.emplace(
                    existing_identity_key(tracked.metadata, "virtual-knob"),
                    tracked.external_id);
            } else if (tracked.metadata.has_unit(metadata_input)
                && tracked.metadata.has_unit(metadata_virtual)
                && tracked.metadata.has_unit(metadata_event)) {
                virtual_event_input_external_ids_by_key.emplace(
                    existing_identity_key(tracked.metadata, "virtual-event-input"),
                    tracked.external_id);
            } else if (tracked.metadata.has_unit(metadata_input)
                && tracked.metadata.has_unit(metadata_sample)) {
                sample_input_external_ids_by_key.emplace(
                    existing_identity_key(tracked.metadata, "sample-input"),
                    tracked.external_id);
            } else if (tracked.metadata.has_unit(metadata_input)
                && tracked.metadata.has_unit(metadata_event)) {
                event_input_external_ids_by_key.emplace(
                    existing_identity_key(tracked.metadata, "event-input"),
                    tracked.external_id);
            }
        }
        if (tracked.metadata.has_unit(metadata_graph_output)) {
            if (tracked.metadata.has_unit(metadata_virtual)) {
                virtual_output_external_ids_by_key.emplace(
                    existing_identity_key(tracked.metadata, "virtual-output"),
                    tracked.external_id);
            } else if (tracked.metadata.has_unit(metadata_node_bundle)) {
                node_bundle_output_external_ids_by_key.emplace(
                    existing_identity_key(tracked.metadata, "node-bundle-output"),
                    tracked.external_id);
            }
        }
    }

    for (auto const &port : desired_ports) {
        if (port.port.port_kind == PortKind::sample) {
            if (!port.port.node_bundle_port_ordinal.has_value()) {
                auto const state_key = graph_input_port_key(port.port);
                if (auto const state_it = virtual_sample_knob_states_by_key.find(state_key);
                    state_it != virtual_sample_knob_states_by_key.end()) {
                    if (state_it->second == VirtualSampleKnobState::timeline_lane) {
                        snapshot.sample_input_states.push_back(ProjectSetSampleInputStateRequest{
                            .node_id = port.port.virtual_node_id,
                            .member_ordinal = std::nullopt,
                            .input_ordinal = port.port.port_ordinal,
                            .state = ProjectSampleInputState::timeline_lane,
                            .lane_id = [&]() -> std::optional<InternedString> {
                                auto const key = virtual_knob_key(port);
                                if (auto const it = virtual_sample_knob_external_ids_by_key.find(key);
                                    it != virtual_sample_knob_external_ids_by_key.end()) {
                                    return it->second;
                                }
                                return std::nullopt;
                            }(),
                        });
                    } else {
                        snapshot.sample_input_values.push_back(ProjectSetSampleInputValueRequest{
                            .node_id = port.port.virtual_node_id,
                            .member_ordinal = std::nullopt,
                            .input_ordinal = port.port.port_ordinal,
                            .value = live_input_value_or_locked(
                                port.port.virtual_node_id,
                                port.port.port_ordinal,
                                Sample{0.0f}),
                        });
                    }
                }
                continue;
            }

            auto const state_key = graph_input_port_key(port.port);
            if (auto const state_it = node_bundle_sample_input_states_by_key.find(state_key);
                state_it != node_bundle_sample_input_states_by_key.end()) {
                auto const member_ordinal = *port.port.node_bundle_port_ordinal;
                if (state_it->second == NodeBundleSampleInputState::overridden) {
                    snapshot.sample_input_values.push_back(ProjectSetSampleInputValueRequest{
                        .node_id = port.port.virtual_node_id,
                        .member_ordinal = member_ordinal,
                        .input_ordinal = port.port.port_ordinal,
                        .value = live_input_value_or_locked(
                            port.port.virtual_node_id,
                            member_ordinal,
                            port.port.port_ordinal,
                            Sample{0.0f}),
                    });
                } else {
                    ProjectSampleInputState state = ProjectSampleInputState::default_;
                    switch (state_it->second) {
                    case NodeBundleSampleInputState::overridden:
                        state = ProjectSampleInputState::overridden;
                        break;
                    case NodeBundleSampleInputState::virtual_follow:
                        state = ProjectSampleInputState::virtual_follow;
                        break;
                    case NodeBundleSampleInputState::timeline_lane:
                        state = ProjectSampleInputState::timeline_lane;
                        break;
                    case NodeBundleSampleInputState::disconnected:
                        state = ProjectSampleInputState::disconnected;
                        break;
                    }
                    snapshot.sample_input_states.push_back(ProjectSetSampleInputStateRequest{
                        .node_id = port.port.virtual_node_id,
                        .member_ordinal = member_ordinal,
                        .input_ordinal = port.port.port_ordinal,
                        .state = state,
                        .lane_id = state == ProjectSampleInputState::timeline_lane
                            ? [&]() -> std::optional<InternedString> {
                                auto const key = sample_input_key(port);
                                if (auto const it = sample_input_external_ids_by_key.find(key);
                                    it != sample_input_external_ids_by_key.end()) {
                                    return it->second;
                                }
                                return std::nullopt;
                            }()
                            : std::nullopt,
                    });
                }
            }
            continue;
        }

        auto const event_state_key = graph_input_port_key(port.port);
        if (auto const state_it = node_bundle_event_input_states_by_key.find(event_state_key);
            state_it != node_bundle_event_input_states_by_key.end()) {
            ProjectEventInputState state = ProjectEventInputState::default_;
            switch (state_it->second) {
            case NodeBundleEventInputState::default_:
                state = ProjectEventInputState::default_;
                break;
            case NodeBundleEventInputState::virtual_follow:
                state = ProjectEventInputState::virtual_follow;
                break;
            case NodeBundleEventInputState::timeline_lane:
                state = ProjectEventInputState::timeline_lane;
                break;
            case NodeBundleEventInputState::disconnected:
                state = ProjectEventInputState::disconnected;
                break;
            }
            snapshot.event_input_states.push_back(ProjectSetEventInputStateRequest{
                .node_id = port.port.virtual_node_id,
                .member_ordinal = port.port.node_bundle_port_ordinal,
                .input_ordinal = port.port.port_ordinal,
                .state = state,
                .lane_id = [&]() -> std::optional<InternedString> {
                    if (state == ProjectEventInputState::timeline_lane) {
                        auto const key = event_input_key(port);
                        if (auto const it = event_input_external_ids_by_key.find(key);
                            it != event_input_external_ids_by_key.end()) {
                            return it->second;
                        }
                    }
                    if (state == ProjectEventInputState::virtual_follow) {
                        auto virtual_port = port;
                        virtual_port.port.node_bundle_port_ordinal = std::nullopt;
                        auto const key = virtual_event_input_key(virtual_port);
                        if (auto const it = virtual_event_input_external_ids_by_key.find(key);
                            it != virtual_event_input_external_ids_by_key.end()) {
                            return it->second;
                        }
                    }
                    return std::nullopt;
                }(),
            });
        }
    }

    std::unordered_set<std::string> persisted_public_outputs;
    for (auto const &port : desired_public_output_ports) {
        if (port.source_identity.empty()) continue;
        auto const virtual_key = public_sample_input_state_key(port.instance_id, port.source_identity, std::nullopt);
        auto const member_key = public_sample_input_state_key(port.instance_id, port.source_identity, port.port_ordinal);
        auto const node_id = public_output_node_id(port.instance_id, port.source_identity);
        if (port.port_kind == PortKind::sample) {
            if (persisted_public_outputs.insert("sample:" + virtual_key).second) {
                if (auto const it = public_sample_output_states_by_key.find(virtual_key); it != public_sample_output_states_by_key.end())
                    snapshot.sample_output_states.push_back({.node_id = node_id, .member_ordinal = std::nullopt, .output_ordinal = 0, .state = it->second});
            }
            if (auto const it = public_sample_output_states_by_key.find(member_key); it != public_sample_output_states_by_key.end())
                snapshot.sample_output_states.push_back({.node_id = node_id, .member_ordinal = port.port_ordinal, .output_ordinal = 0, .state = it->second});
        } else {
            if (persisted_public_outputs.insert("event:" + virtual_key).second) {
                if (auto const it = public_event_output_states_by_key.find(virtual_key); it != public_event_output_states_by_key.end())
                    snapshot.event_output_states.push_back({.node_id = node_id, .member_ordinal = std::nullopt, .output_ordinal = 0, .state = it->second});
            }
            if (auto const it = public_event_output_states_by_key.find(member_key); it != public_event_output_states_by_key.end())
                snapshot.event_output_states.push_back({.node_id = node_id, .member_ordinal = port.port_ordinal, .output_ordinal = 0, .state = it->second});
        }
    }

    for (auto const &port : desired_output_ports) {
        auto const output_state_key = graph_input_port_key(port.port);
        if (port.port.port_kind == PortKind::sample) {
            if (port.port.node_bundle_port_ordinal.has_value()) {
                if (auto const it = node_bundle_output_states_by_key.find(output_state_key);
                    it != node_bundle_output_states_by_key.end()) {
                    snapshot.sample_output_states.push_back(ProjectSetSampleOutputStateRequest{
                        .node_id = port.port.virtual_node_id,
                        .member_ordinal = port.port.node_bundle_port_ordinal,
                        .output_ordinal = port.port.port_ordinal,
                        .state = it->second == NodeBundleOutputState::virtual_port
                            ? ProjectSampleOutputState::virtual_port
                            : ProjectSampleOutputState::timeline_lane,
                        .lane_id = it->second == NodeBundleOutputState::timeline_lane
                            ? [&]() -> std::optional<InternedString> {
                                auto const key = output_identity_key(port, "node-bundle-output");
                                if (auto const lane_it = node_bundle_output_external_ids_by_key.find(key);
                                    lane_it != node_bundle_output_external_ids_by_key.end()) {
                                    return lane_it->second;
                                }
                                return std::nullopt;
                            }()
                            : std::nullopt,
                    });
                }
            } else if (auto const it = virtual_output_states_by_key.find(output_state_key);
                it != virtual_output_states_by_key.end()) {
                (void)it;
                snapshot.sample_output_states.push_back(ProjectSetSampleOutputStateRequest{
                    .node_id = port.port.virtual_node_id,
                    .member_ordinal = std::nullopt,
                    .output_ordinal = port.port.port_ordinal,
                    .state = ProjectSampleOutputState::timeline_lane,
                    .lane_id = [&]() -> std::optional<InternedString> {
                        auto const key = output_identity_key(port, "virtual-output");
                        if (auto const lane_it = virtual_output_external_ids_by_key.find(key);
                            lane_it != virtual_output_external_ids_by_key.end()) {
                            return lane_it->second;
                        }
                        return std::nullopt;
                    }(),
                });
            }
            continue;
        }

        if (port.port.node_bundle_port_ordinal.has_value()) {
            if (auto const it = node_bundle_output_states_by_key.find(output_state_key);
                it != node_bundle_output_states_by_key.end()) {
                snapshot.event_output_states.push_back(ProjectSetEventOutputStateRequest{
                    .node_id = port.port.virtual_node_id,
                    .member_ordinal = port.port.node_bundle_port_ordinal,
                    .output_ordinal = port.port.port_ordinal,
                    .state = it->second == NodeBundleOutputState::virtual_port
                        ? ProjectEventOutputState::virtual_port
                        : ProjectEventOutputState::timeline_lane,
                    .lane_id = it->second == NodeBundleOutputState::timeline_lane
                        ? [&]() -> std::optional<InternedString> {
                            auto const key = output_identity_key(port, "node-bundle-output");
                            if (auto const lane_it = node_bundle_output_external_ids_by_key.find(key);
                                lane_it != node_bundle_output_external_ids_by_key.end()) {
                                return lane_it->second;
                            }
                            return std::nullopt;
                        }()
                        : std::nullopt,
                });
            }
        } else if (auto const it = virtual_output_states_by_key.find(output_state_key);
            it != virtual_output_states_by_key.end()) {
            (void)it;
            snapshot.event_output_states.push_back(ProjectSetEventOutputStateRequest{
                .node_id = port.port.virtual_node_id,
                .member_ordinal = std::nullopt,
                .output_ordinal = port.port.port_ordinal,
                .state = ProjectEventOutputState::timeline_lane,
                .lane_id = [&]() -> std::optional<InternedString> {
                    auto const key = output_identity_key(port, "virtual-output");
                    if (auto const lane_it = virtual_output_external_ids_by_key.find(key);
                        lane_it != virtual_output_external_ids_by_key.end()) {
                        return lane_it->second;
                    }
                    return std::nullopt;
                }(),
            });
        }
    }

    return snapshot;
}


} // namespace iv
