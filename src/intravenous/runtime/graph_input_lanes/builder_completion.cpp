#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes/details.h>

#include <algorithm>

namespace iv {
using namespace graph_input_lanes_details;
namespace {

std::string local_virtual_node_id(
    std::string_view instance_id,
    std::string_view runtime_id)
{
    auto const prefix = std::string(instance_id) + "\x1fvirtual:";
    if (!runtime_id.starts_with(prefix)) {
        details::error("runtime virtual-node identity is not owned by its instance");
    }
    return std::string(runtime_id.substr(prefix.size()));
}

void bind_none(RuntimeSampleInputBinding& binding)
{
    binding.timeline_lane = {};
    binding.source_channel = 0;
    binding.mode = RuntimeSampleInputMode::none;
}

void bind_scalar(RuntimeSampleInputBinding& binding, Sample value)
{
    binding.value = value;
    binding.timeline_lane = {};
    binding.source_channel = 0;
    binding.mode = RuntimeSampleInputMode::scalar;
}

void bind_timeline(
    RuntimeSampleInputBinding& binding,
    LaneId lane,
    size_t source_channel = 0)
{
    binding.timeline_lane = lane;
    binding.source_channel = source_channel;
    binding.mode = RuntimeSampleInputMode::timeline;
}

void set_event_source(RuntimeEventInputBinding& binding, LaneId lane)
{
    binding.timeline_lane = lane;
}

} // namespace

void GraphInputLanes::sync_runtime_bindings_locked(
    std::string const& instance_id)
{
    auto const runtime_it = runtime_bindings_by_instance_id.find(instance_id);
    if (runtime_it == runtime_bindings_by_instance_id.end() || !runtime_it->second) {
        return;
    }
    auto& runtime = *runtime_it->second;

    auto const desired_inputs_it = desired_ports_by_instance_id.find(instance_id);
    if (desired_inputs_it != desired_ports_by_instance_id.end()) {
        auto const& ports = desired_inputs_it->second;
        for (auto const& port : ports) {
            if (!port.port.node_bundle_port_ordinal.has_value()) {
                continue;
            }
            auto const local_id = local_virtual_node_id(
                instance_id, port.port.virtual_node_id);
            auto const member = *port.port.node_bundle_port_ordinal;

            if (port.port.port_kind == PortKind::sample) {
                sample_input_default_values[
                    sample_default_value_key(instance_id, port.port)] =
                    port.default_value;
                (void)ensure_live_input_value_initialized_locked(
                    port.port.virtual_node_id,
                    port.port.port_ordinal,
                    port.default_value);
                (void)ensure_live_input_value_initialized_locked(
                    node_bundle_key(port.port.virtual_node_id, member),
                    port.port.port_ordinal,
                    live_input_value_or_locked(
                        port.port.virtual_node_id,
                        port.port.port_ordinal,
                        port.default_value));

                auto binding = runtime.sample_input(runtime_virtual_port_key(
                    true, PortKind::sample, local_id, member,
                    port.port.port_ordinal));
                auto state = port.authored_connected
                    ? NodeBundleSampleInputState::disconnected
                    : NodeBundleSampleInputState::virtual_follow;
                if (auto const it = node_bundle_sample_input_states_by_key.find(
                        graph_input_port_key(port.port));
                    it != node_bundle_sample_input_states_by_key.end()) {
                    state = it->second;
                }

                switch (state) {
                case NodeBundleSampleInputState::disconnected:
                    bind_none(*binding);
                    break;
                case NodeBundleSampleInputState::overridden:
                    bind_scalar(
                        *binding,
                        live_input_value_or_locked(
                            port.port.virtual_node_id,
                            member,
                            port.port.port_ordinal,
                            port.default_value));
                    break;
                case NodeBundleSampleInputState::timeline_lane:
                    bind_timeline(*binding, stable_lane_id_for_key(sample_input_key(port)));
                    break;
                case NodeBundleSampleInputState::virtual_follow: {
                    auto virtual_port = port;
                    virtual_port.port.node_bundle_port_ordinal = std::nullopt;
                    auto const virtual_state_it =
                        virtual_sample_knob_states_by_key.find(
                            graph_input_port_key(virtual_port.port));
                    auto const virtual_state =
                        virtual_state_it == virtual_sample_knob_states_by_key.end()
                        ? VirtualSampleKnobState::overridden
                        : virtual_state_it->second;
                    if (virtual_state == VirtualSampleKnobState::timeline_lane) {
                        bind_timeline(
                            *binding,
                            stable_lane_id_for_key(virtual_knob_key(virtual_port)));
                    } else {
                        bind_scalar(
                            *binding,
                            live_input_value_or_locked(
                                port.port.virtual_node_id,
                                port.port.port_ordinal,
                                port.default_value));
                    }
                    break;
                }
                }
                continue;
            }

            auto binding = runtime.event_input(runtime_virtual_port_key(
                true, PortKind::event, local_id, member,
                port.port.port_ordinal));
            auto state = port.authored_connected
                ? NodeBundleEventInputState::disconnected
                : NodeBundleEventInputState::virtual_follow;
            if (auto const it = node_bundle_event_input_states_by_key.find(
                    graph_input_port_key(port.port));
                it != node_bundle_event_input_states_by_key.end()) {
                state = it->second;
            }
            if (state == NodeBundleEventInputState::default_) {
                state = port.authored_connected
                    ? NodeBundleEventInputState::disconnected
                    : NodeBundleEventInputState::virtual_follow;
            }
            if (state == NodeBundleEventInputState::timeline_lane) {
                set_event_source(*binding, stable_lane_id_for_key(event_input_key(port)));
            } else if (state == NodeBundleEventInputState::virtual_follow) {
                auto virtual_port = port;
                virtual_port.port.node_bundle_port_ordinal = std::nullopt;
                set_event_source(
                    *binding,
                    stable_lane_id_for_key(virtual_event_input_key(virtual_port)));
            } else {
                set_event_source(*binding, LaneId{});
            }
        }
    }

    auto const public_inputs_it =
        desired_public_input_ports_by_instance_id.find(instance_id);
    if (public_inputs_it != desired_public_input_ports_by_instance_id.end()) {
        for (auto const& port : public_inputs_it->second) {
            if (port.port_kind == PortKind::sample) {
                auto const lane = effective_public_sample_input_lane_locked(port);
                auto const virtual_key = public_sample_input_state_key(
                    instance_id, port.source_identity, std::nullopt);
                auto const state_it = public_sample_input_states_by_key.find(virtual_key);
                auto const state = port.source_identity.empty()
                    ? ProjectSampleInputState::timeline_lane
                    : state_it == public_sample_input_states_by_key.end()
                        ? ProjectSampleInputState::timeline_lane
                        : state_it->second;
                for (auto const& channel : port.channels) {
                    if (!channel.port_ordinal) continue;
                    auto binding = runtime.sample_input(runtime_public_port_key(
                        true, PortKind::sample, *channel.port_ordinal));
                    if (lane && *lane) {
                        bind_timeline(*binding, *lane);
                    } else if (state == ProjectSampleInputState::overridden) {
                        auto& value = ensure_public_sample_input_value_locked(
                            instance_id, port.source_identity, port.default_value);
                        bind_scalar(
                            *binding,
                            value
                        );
                    } else {
                        bind_none(*binding);
                    }
                }
            } else {
                auto binding = runtime.event_input(runtime_public_port_key(
                    true, PortKind::event, port.port_ordinal));
                auto const lane = effective_public_event_input_lane_locked(port);
                set_event_source(*binding, lane.value_or(LaneId{}));
            }
        }
    }

    auto const desired_outputs_it =
        desired_output_ports_by_instance_id.find(instance_id);
    if (desired_outputs_it != desired_output_ports_by_instance_id.end()) {
        for (auto const& port : desired_outputs_it->second) {
            auto const local_id = local_virtual_node_id(
                instance_id, port.port.virtual_node_id);
            auto binding = runtime.output(runtime_virtual_port_key(
                false, port.port.port_kind, local_id,
                port.port.node_bundle_port_ordinal,
                port.port.port_ordinal));
            LaneId lane {};
            bool include_in_aggregate = false;
            if (port.port.node_bundle_port_ordinal.has_value()) {
                auto const state = effective_node_bundle_output_state_locked(port);
                if (state == NodeBundleOutputState::timeline_lane) {
                    lane = stable_lane_id_for_key(output_identity_key(
                        port, "node-bundle-output"));
                } else if (state == NodeBundleOutputState::virtual_port) {
                    include_in_aggregate = true;
                }
            } else if (virtual_output_is_timeline_lane_locked(port)) {
                lane = stable_lane_id_for_key(output_identity_key(
                    port, "virtual-output"));
            }
            if (lane) {
                if (port.port.port_kind == PortKind::sample)
                    output_blocks_.prepare_sample(lane);
                else
                    output_blocks_.prepare_event(lane);
            }
            binding->include_in_aggregate = include_in_aggregate;
            binding->target_lane = lane;
        }
    }

    auto const public_outputs_it =
        desired_public_output_ports_by_instance_id.find(instance_id);
    if (public_outputs_it != desired_public_output_ports_by_instance_id.end()) {
        std::unordered_set<std::string> visited;
        for (auto const& port : public_outputs_it->second) {
            auto const key = runtime_public_port_key(
                false, port.port_kind, port.port_ordinal);
            if (!visited.insert(key).second) continue;
            bool enabled = port.source_identity.empty();
            for (auto const& candidate : public_outputs_it->second) {
                if (candidate.port_kind != port.port_kind ||
                    candidate.port_ordinal != port.port_ordinal) {
                    continue;
                }
                if (candidate.source_identity.empty()) {
                    enabled = true;
                    break;
                }
                auto const virtual_key = public_sample_input_state_key(
                    instance_id, candidate.source_identity, std::nullopt);
                auto const member_key = public_sample_input_state_key(
                    instance_id, candidate.source_identity,
                    candidate.node_bundle_port_ordinal.value_or(
                        candidate.port_ordinal));
                if (candidate.port_kind == PortKind::sample) {
                    auto const virtual_it =
                        public_sample_output_states_by_key.find(virtual_key);
                    auto const virtual_connected =
                        virtual_it == public_sample_output_states_by_key.end() ||
                        virtual_it->second == ProjectSampleOutputState::timeline_lane;
                    auto const member_it =
                        public_sample_output_states_by_key.find(member_key);
                    auto const member_state =
                        member_it == public_sample_output_states_by_key.end()
                        ? ProjectSampleOutputState::virtual_port
                        : member_it->second;
                    enabled |= member_state == ProjectSampleOutputState::timeline_lane ||
                        (member_state == ProjectSampleOutputState::virtual_port &&
                         virtual_connected);
                } else {
                    auto const virtual_it =
                        public_event_output_states_by_key.find(virtual_key);
                    auto const virtual_connected =
                        virtual_it == public_event_output_states_by_key.end() ||
                        virtual_it->second == ProjectEventOutputState::timeline_lane;
                    auto const member_it =
                        public_event_output_states_by_key.find(member_key);
                    auto const member_state =
                        member_it == public_event_output_states_by_key.end()
                        ? ProjectEventOutputState::virtual_port
                        : member_it->second;
                    enabled |= member_state == ProjectEventOutputState::timeline_lane ||
                        (member_state == ProjectEventOutputState::virtual_port &&
                         virtual_connected);
                }
            }
            auto binding = runtime.output(key);
            auto const lane = enabled ? public_graph_port_lane_for(port) : LaneId{};
            if (lane) {
                if (port.port_kind == PortKind::sample)
                    output_blocks_.prepare_sample(lane);
                else
                    output_blocks_.prepare_event(lane);
            }
            binding->target_lane = lane;
        }
    }
}

void GraphInputLanes::schedule_runtime_binding_sync_locked(
    std::string const& instance_id)
{
    if (runtime_bindings_by_instance_id.contains(instance_id))
        pending_runtime_binding_syncs.insert(instance_id);
}

std::vector<LaneId> GraphInputLanes::prerequisite_lanes_for_instance_locked(
    std::string const& instance_id) const
{
    std::vector<LaneId> result;
    auto append = [&](LaneId lane) {
        if (lane && !std::ranges::contains(result, lane)) result.push_back(lane);
    };
    if (auto const it = desired_ports_by_instance_id.find(instance_id);
        it != desired_ports_by_instance_id.end()) {
        for (auto const& port : it->second) {
            if (!port.port.node_bundle_port_ordinal) continue;
            if (port.port.port_kind == PortKind::sample) {
                auto const state_it = node_bundle_sample_input_states_by_key.find(
                    graph_input_port_key(port.port));
                auto const state = state_it == node_bundle_sample_input_states_by_key.end()
                    ? (port.authored_connected
                        ? NodeBundleSampleInputState::disconnected
                        : NodeBundleSampleInputState::virtual_follow)
                    : state_it->second;
                if (state == NodeBundleSampleInputState::timeline_lane) {
                    append(stable_lane_id_for_key(sample_input_key(port)));
                } else if (state == NodeBundleSampleInputState::virtual_follow) {
                    auto virtual_port = port;
                    virtual_port.port.node_bundle_port_ordinal = std::nullopt;
                    auto const virtual_it = virtual_sample_knob_states_by_key.find(
                        graph_input_port_key(virtual_port.port));
                    if (virtual_it != virtual_sample_knob_states_by_key.end() &&
                        virtual_it->second == VirtualSampleKnobState::timeline_lane) {
                        append(stable_lane_id_for_key(virtual_knob_key(virtual_port)));
                    }
                }
            } else {
                auto const state_it = node_bundle_event_input_states_by_key.find(
                    graph_input_port_key(port.port));
                auto const state = state_it == node_bundle_event_input_states_by_key.end()
                    ? (port.authored_connected
                        ? NodeBundleEventInputState::disconnected
                        : NodeBundleEventInputState::virtual_follow)
                    : state_it->second;
                if (state == NodeBundleEventInputState::timeline_lane) {
                    append(stable_lane_id_for_key(event_input_key(port)));
                } else if (state == NodeBundleEventInputState::virtual_follow) {
                    auto virtual_port = port;
                    virtual_port.port.node_bundle_port_ordinal = std::nullopt;
                    append(stable_lane_id_for_key(virtual_event_input_key(virtual_port)));
                }
            }
        }
    }
    if (auto const it = desired_public_input_ports_by_instance_id.find(instance_id);
        it != desired_public_input_ports_by_instance_id.end()) {
        for (auto const& port : it->second) {
            auto const lane = port.port_kind == PortKind::sample
                ? effective_public_sample_input_lane_locked(port)
                : effective_public_event_input_lane_locked(port);
            if (lane) append(*lane);
        }
    }
    std::ranges::sort(result, {}, &LaneId::value);
    return result;
}

} // namespace iv
