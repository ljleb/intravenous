#include <intravenous/runtime/graph_input_lanes.h>

#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_lane_nodes/controls.h>
#include <intravenous/runtime/graph_input_lanes_execution_edges_events.h>

#include <array>
#include <charconv>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace iv {
namespace {
constexpr std::string_view metadata_dsp_graph = "dsp_graph";
constexpr std::string_view metadata_graph_input = "dsp_graph.graph_input";
constexpr std::string_view metadata_input = "dsp_graph.input";
constexpr std::string_view metadata_knob = "dsp_graph.knob";
constexpr std::string_view metadata_logical = "dsp_graph.logical";
constexpr std::string_view metadata_concrete = "dsp_graph.concrete";
constexpr std::string_view metadata_sample = "dsp_graph.sample";
constexpr std::string_view metadata_event = "dsp_graph.event";
constexpr std::string_view metadata_module_instance_id = "dsp_graph.module_instance_id";
constexpr std::string_view metadata_logical_node_id = "dsp_graph.logical_node_id";
constexpr std::string_view metadata_concrete_node_id = "dsp_graph.concrete_node_id";
constexpr std::string_view metadata_port_kind = "dsp_graph.port_kind";
constexpr std::string_view metadata_port_ordinal = "dsp_graph.port_ordinal";
constexpr std::string_view metadata_member_ordinal = "dsp_graph.member_ordinal";

int local_hash_string(std::string const &value)
{
    return static_cast<int>(std::hash<std::string>{}(value));
}

void append_graph_input_port_descriptors(
    std::vector<GraphInputLanes::DesiredGraphInputPort> &ports,
    std::string const &instance_id,
    int module_instance_id,
    std::string const &logical_node_id,
    std::optional<size_t> concrete_member_ordinal,
    PortKind port_kind,
    std::span<IntrospectionPortInfo const> logical_ports)
{
    ports.reserve(ports.size() + logical_ports.size());
    for (auto const &port : logical_ports) {
        ports.push_back(GraphInputLanes::DesiredGraphInputPort{
            .instance_id = instance_id,
            .module_instance_id = module_instance_id,
            .port = GraphInputPortDescriptor{
                .logical_node_id = logical_node_id,
                .concrete_member_ordinal = concrete_member_ordinal,
                .port_kind = port_kind,
                .port_ordinal = port.ordinal,
                .port_name = port.name,
                .port_type = port.type,
            },
            .default_connected = port.connectivity != LogicalPortConnectivity::disconnected,
        });
    }
}

bool batch_has_changes(TimelineLaneBatchUpdate const &batch)
{
    return !batch.upserts.empty()
        || !batch.removals.empty()
        || !batch.connections_to_remove.empty()
        || !batch.connections_to_add.empty()
        || !batch.hierarchy_removals.empty()
        || !batch.hierarchy_additions.empty();
}

std::string logical_knob_key(GraphInputLanes::DesiredGraphInputPort const &port)
{
    auto const logical_node_id = local_hash_string(port.port.logical_node_id);
    auto const concrete_node_id = local_hash_string(
        port.port.logical_node_id
        + "\x1f"
        + "member:"
        + (port.port.concrete_member_ordinal.has_value()
            ? std::to_string(*port.port.concrete_member_ordinal)
            : std::string("logical")));

    return "logical-knob:instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "logical:" + std::to_string(logical_node_id)
        + "\x1f" + "concrete:" + std::to_string(concrete_node_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal);
}

std::string logical_event_input_key(GraphInputLanes::DesiredGraphInputPort const &port)
{
    auto const logical_node_id = local_hash_string(port.port.logical_node_id);
    auto const concrete_node_id = local_hash_string(
        port.port.logical_node_id
        + "\x1f"
        + "member:logical");

    return "logical-event-input:instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "logical:" + std::to_string(logical_node_id)
        + "\x1f" + "concrete:" + std::to_string(concrete_node_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal);
}

std::string sample_input_key(GraphInputLanes::DesiredGraphInputPort const &port)
{
    auto const logical_node_id = local_hash_string(port.port.logical_node_id);
    auto const concrete_node_id = local_hash_string(
        port.port.logical_node_id
        + "\x1f"
        + "member:"
        + (port.port.concrete_member_ordinal.has_value()
            ? std::to_string(*port.port.concrete_member_ordinal)
            : std::string("logical")));

    return "sample-input:instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "logical:" + std::to_string(logical_node_id)
        + "\x1f" + "concrete:" + std::to_string(concrete_node_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal);
}

std::string event_input_key(GraphInputLanes::DesiredGraphInputPort const &port)
{
    auto const logical_node_id = local_hash_string(port.port.logical_node_id);
    auto const concrete_node_id = local_hash_string(
        port.port.logical_node_id
        + "\x1f"
        + "member:"
        + (port.port.concrete_member_ordinal.has_value()
            ? std::to_string(*port.port.concrete_member_ordinal)
            : std::string("logical")));

    return "event-input:instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "logical:" + std::to_string(logical_node_id)
        + "\x1f" + "concrete:" + std::to_string(concrete_node_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal);
}

std::string existing_identity_key(
    LaneMetadata const &metadata,
    std::string_view role)
{
    auto const module_instance_id = metadata.int_value(metadata_module_instance_id);
    auto const logical_node_id = metadata.int_value(metadata_logical_node_id);
    auto const concrete_node_id = metadata.int_value(metadata_concrete_node_id);
    auto const port_kind = metadata.int_value(metadata_port_kind);
    auto const port_ordinal = metadata.int_value(metadata_port_ordinal);
    if (!module_instance_id.has_value()
        || !logical_node_id.has_value()
        || !concrete_node_id.has_value()
        || !port_kind.has_value()
        || !port_ordinal.has_value()) {
        return {};
    }

    return std::string(role)
        + ":instance:" + std::to_string(*module_instance_id)
        + "\x1f" + "logical:" + std::to_string(*logical_node_id)
        + "\x1f" + "concrete:" + std::to_string(*concrete_node_id)
        + "\x1f" + "kind:" + std::to_string(*port_kind)
        + "\x1f" + "ordinal:" + std::to_string(*port_ordinal);
}

TypeErasedLaneNode make_sample_input_node(Sample default_value)
{
    return TypeErasedLaneNode(GraphSampleInputLaneNode{
        .default_value = default_value,
    });
}

GraphInputPortDescriptor sample_port_descriptor(
    GraphBuilder::LogicalSampleInput const &input,
    std::optional<size_t> member_ordinal)
{
    return GraphInputPortDescriptor{
        .logical_node_id = input.logical_node_id,
        .concrete_member_ordinal = member_ordinal,
        .port_kind = PortKind::sample,
        .port_ordinal = input.target.port,
        .port_name = input.config.name,
        .port_type = "sample",
    };
}

GraphInputPortDescriptor event_port_descriptor(
    GraphBuilder::LogicalEventInput const &input,
    std::optional<size_t> member_ordinal)
{
    return GraphInputPortDescriptor{
        .logical_node_id = input.logical_node_id,
        .concrete_member_ordinal = member_ordinal,
        .port_kind = PortKind::event,
        .port_ordinal = input.target.port,
        .port_name = input.config.name,
        .port_type = details::event_type_name(input.config.type),
    };
}

} // namespace

std::vector<GraphInputLanes::DesiredGraphInputPort>
GraphInputLanes::graph_input_port_descriptors_for(
    IvModuleInstance const &instance)
{
    std::vector<DesiredGraphInputPort> ports;
    auto const module_instance_id = module_instance_numeric_id(instance.instance_id);
    for (auto const &node : instance.introspection.logical_nodes) {
        append_graph_input_port_descriptors(
            ports,
            instance.instance_id,
            module_instance_id,
            node.id,
            std::nullopt,
            PortKind::sample,
            node.sample_inputs);
        append_graph_input_port_descriptors(
            ports,
            instance.instance_id,
            module_instance_id,
            node.id,
            std::nullopt,
            PortKind::event,
            node.event_inputs);
        for (auto const &member : node.members) {
            append_graph_input_port_descriptors(
                ports,
                instance.instance_id,
                module_instance_id,
                node.id,
                member.ordinal,
                PortKind::sample,
                member.sample_inputs);
            append_graph_input_port_descriptors(
                ports,
                instance.instance_id,
                module_instance_id,
                node.id,
                member.ordinal,
                PortKind::event,
                member.event_inputs);
        }
    }
    return ports;
}

int GraphInputLanes::module_instance_numeric_id(std::string_view instance_id)
{
    auto const separator = instance_id.rfind(':');
    if (separator != std::string_view::npos) {
        int parsed = 0;
        auto const numeric = instance_id.substr(separator + 1);
        auto const [ptr, ec] = std::from_chars(
            numeric.data(),
            numeric.data() + numeric.size(),
            parsed);
        if (ec == std::errc{} && ptr == numeric.data() + numeric.size()) {
            return parsed;
        }
    }
    return hash_string(std::string(instance_id));
}

int GraphInputLanes::hash_string(std::string const &value)
{
    return static_cast<int>(std::hash<std::string>{}(value));
}

std::string GraphInputLanes::concrete_key(std::string_view logical_node_id, size_t member_ordinal)
{
    return concrete_key_prefix(logical_node_id) + std::to_string(member_ordinal);
}

std::string GraphInputLanes::concrete_key_prefix(std::string_view logical_node_id)
{
    return std::string(logical_node_id) + "\x1fmember:";
}

std::string GraphInputLanes::concrete_override_key(
    std::string_view logical_node_id,
    size_t member_ordinal,
    size_t input_ordinal)
{
    return concrete_key(logical_node_id, member_ordinal) + "\x1finput:" + std::to_string(input_ordinal);
}

std::string GraphInputLanes::desired_port_key(DesiredGraphInputPort const &port)
{
    auto const logical_node_id = hash_string(port.port.logical_node_id);
    auto const concrete_node_id = hash_string(
        port.port.logical_node_id
        + "\x1fmember:"
        + (port.port.concrete_member_ordinal.has_value()
            ? std::to_string(*port.port.concrete_member_ordinal)
            : std::string("logical")));

    return "instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "logical:" + std::to_string(logical_node_id)
        + "\x1f" + "concrete:" + std::to_string(concrete_node_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal);
}

std::string GraphInputLanes::graph_input_port_key(GraphInputPortDescriptor const &port)
{
    std::string key = std::to_string(hash_string(port.logical_node_id));
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

std::string GraphInputLanes::sample_default_value_key(
    std::string_view instance_id,
    GraphInputPortDescriptor const &port)
{
    return std::string(instance_id) + "\x1f" + graph_input_port_key(port);
}

std::string GraphInputLanes::instance_port_state_key(
    std::string_view instance_id,
    GraphInputPortDescriptor const &port)
{
    (void)instance_id;
    return graph_input_port_key(port);
}

GraphInputPortDescriptor GraphInputLanes::sample_input_descriptor(
    std::string const &node_id,
    std::optional<size_t> member_ordinal,
    size_t input_ordinal)
{
    return GraphInputPortDescriptor{
        .logical_node_id = node_id,
        .concrete_member_ordinal = member_ordinal,
        .port_kind = PortKind::sample,
        .port_ordinal = input_ordinal,
        .port_name = "",
        .port_type = "sample",
    };
}

LaneMetadata GraphInputLanes::graph_input_metadata(
    DesiredGraphInputPort const &port,
    bool knob,
    bool logical,
    bool concrete,
    bool sample,
    bool event)
{
    LaneMetadata metadata;
    metadata.set_unit(std::string(metadata_dsp_graph));
    metadata.set_unit(std::string(metadata_graph_input));
    if (knob) {
        metadata.set_unit(std::string(metadata_knob));
    } else {
        metadata.set_unit(std::string(metadata_input));
    }
    if (logical) {
        metadata.set_unit(std::string(metadata_logical));
    }
    if (concrete) {
        metadata.set_unit(std::string(metadata_concrete));
    }
    if (sample) {
        metadata.set_unit(std::string(metadata_sample));
    }
    if (event) {
        metadata.set_unit(std::string(metadata_event));
    }
    metadata.set_int(std::string(metadata_module_instance_id), port.module_instance_id);
    metadata.set_int(std::string(metadata_logical_node_id), hash_string(port.port.logical_node_id));
    metadata.set_int(
        std::string(metadata_concrete_node_id),
        hash_string(
            port.port.logical_node_id
            + "\x1fmember:"
            + (port.port.concrete_member_ordinal.has_value()
                ? std::to_string(*port.port.concrete_member_ordinal)
                : std::string("logical"))));
    metadata.set_int(
        std::string(metadata_port_kind),
        static_cast<int>(port.port.port_kind == PortKind::event));
    metadata.set_int(
        std::string(metadata_port_ordinal),
        static_cast<int>(port.port.port_ordinal));
    if (port.port.concrete_member_ordinal.has_value()) {
        metadata.set_int(
            std::string(metadata_member_ordinal),
            static_cast<int>(*port.port.concrete_member_ordinal));
    }
    return metadata;
}

bool GraphInputLanes::lane_metadata_matches_port(
    LaneMetadata const &metadata,
    DesiredGraphInputPort const &port)
{
    auto const logical_node_id = metadata.int_value(metadata_logical_node_id);
    auto const concrete_node_id = metadata.int_value(metadata_concrete_node_id);
    auto const port_kind = metadata.int_value(metadata_port_kind);
    auto const port_ordinal = metadata.int_value(metadata_port_ordinal);
    if (!logical_node_id.has_value()
        || !concrete_node_id.has_value()
        || !port_kind.has_value()
        || !port_ordinal.has_value()) {
        return false;
    }

    auto const expected_logical = hash_string(port.port.logical_node_id);
    auto const expected_concrete = hash_string(
        port.port.logical_node_id
        + "\x1fmember:"
        + (port.port.concrete_member_ordinal.has_value()
            ? std::to_string(*port.port.concrete_member_ordinal)
            : std::string("logical")));

    return *logical_node_id == expected_logical
        && *concrete_node_id == expected_concrete
        && *port_kind == static_cast<int>(port.port.port_kind == PortKind::event)
        && *port_ordinal == static_cast<int>(port.port.port_ordinal);
}

bool GraphInputLanes::has_concrete_descriptor_for_port(
    std::span<DesiredGraphInputPort const> ports,
    DesiredGraphInputPort const &logical_port)
{
    for (auto const &port : ports) {
        if (port.instance_id != logical_port.instance_id) {
            continue;
        }
        if (port.port.logical_node_id != logical_port.port.logical_node_id) {
            continue;
        }
        if (port.port.port_kind != logical_port.port.port_kind) {
            continue;
        }
        if (port.port.port_ordinal != logical_port.port.port_ordinal) {
            continue;
        }
        if (port.port.concrete_member_ordinal.has_value()) {
            return true;
        }
    }
    return false;
}

std::vector<Sample*>& GraphInputLanes::ensure_live_input_slots_locked(
    std::string_view key,
    size_t input_ordinal)
{
    auto &slots = live_inputs[std::string(key)];
    if (slots.size() <= input_ordinal) {
        slots.resize(input_ordinal + 1);
    }
    return slots[input_ordinal];
}

std::atomic<Sample::storage>& GraphInputLanes::ensure_live_input_value_locked(
    std::string_view key,
    size_t input_ordinal)
{
    auto &values = live_input_values[std::string(key)];
    if (values.size() <= input_ordinal) {
        values.resize(input_ordinal + 1);
    }
    if (!values[input_ordinal]) {
        values[input_ordinal] =
            std::make_unique<std::atomic<Sample::storage>>(Sample{0.0f}.value);
    }
    return *values[input_ordinal];
}

std::atomic<Sample::storage>& GraphInputLanes::ensure_live_input_value_initialized_locked(
    std::string_view key,
    size_t input_ordinal,
    Sample initial_value)
{
    auto &values = live_input_values[std::string(key)];
    if (values.size() <= input_ordinal) {
        values.resize(input_ordinal + 1);
    }
    if (!values[input_ordinal]) {
        values[input_ordinal] =
            std::make_unique<std::atomic<Sample::storage>>(initial_value.value);
    }
    return *values[input_ordinal];
}

Sample GraphInputLanes::live_input_value_or_locked(
    std::string_view logical_node_id,
    size_t input_ordinal,
    Sample fallback) const
{
    auto it = live_input_values.find(std::string(logical_node_id));
    if (it == live_input_values.end() || it->second.size() <= input_ordinal) {
        return fallback;
    }
    if (!it->second[input_ordinal]) {
        return fallback;
    }
    return Sample{it->second[input_ordinal]->load(std::memory_order_relaxed)};
}

Sample GraphInputLanes::live_input_value_or_locked(
    std::string_view logical_node_id,
    size_t member_ordinal,
    size_t input_ordinal,
    Sample fallback) const
{
    auto const override = concrete_override_key(logical_node_id, member_ordinal, input_ordinal);
    auto const key = concrete_key(logical_node_id, member_ordinal);
    if (concrete_live_input_overrides.contains(override)) {
        auto it = live_input_values.find(key);
        if (it != live_input_values.end()
            && it->second.size() > input_ordinal
            && it->second[input_ordinal]) {
            return Sample{it->second[input_ordinal]->load(std::memory_order_relaxed)};
        }
    }
    return live_input_value_or_locked(logical_node_id, input_ordinal, fallback);
}

std::atomic<Sample::storage> const* GraphInputLanes::live_input_value_ptr_or_locked(
    std::string_view key,
    size_t input_ordinal)
{
    return &ensure_live_input_value_locked(key, input_ordinal);
}

void GraphInputLanes::mark_instances_requiring_rebuild_locked(
    std::string_view logical_node_id,
    std::optional<size_t> member_ordinal,
    size_t input_ordinal)
{
    for (auto const &port : desired_ports) {
        if (port.port.port_kind != PortKind::sample) {
            continue;
        }
        if (port.port.logical_node_id != logical_node_id) {
            continue;
        }
        if (port.port.port_ordinal != input_ordinal) {
            continue;
        }
        if (member_ordinal.has_value()) {
            if (port.port.concrete_member_ordinal != member_ordinal) {
                continue;
            }
        }
        pending_rebuild_instance_ids.insert(port.instance_id);
    }
}

void GraphInputLanes::mark_instances_requiring_rebuild_for_logical_sample_input_locked(
    std::string_view logical_node_id,
    size_t input_ordinal)
{
    mark_instances_requiring_rebuild_locked(
        logical_node_id,
        std::nullopt,
        input_ordinal);
}

std::optional<GraphInputLanes::DesiredGraphInputPort> GraphInputLanes::find_desired_port_locked(
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

GraphInputLaneBindings GraphInputLanes::reconcile_ports_locked(TimelineLaneBatchUpdate *batch)
{
    GraphInputLaneBindings result;
    std::unordered_map<std::string, ExistingTrackedLane> logical_sample_knobs_by_key;
    std::unordered_map<std::string, ExistingTrackedLane> logical_event_inputs_by_key;
    std::unordered_map<std::string, ExistingTrackedLane> sample_inputs_by_key;
    std::unordered_map<std::string, ExistingTrackedLane> event_inputs_by_key;
    std::unordered_set<std::uint64_t> used_lane_ids;

    for (auto const &tracked : tracked_lanes) {
        lane_ids.observe(tracked.lane);
        if (!tracked.metadata.has_unit(metadata_graph_input)) {
            continue;
        }
        if (tracked.metadata.has_unit(metadata_knob)
            && tracked.metadata.has_unit(metadata_logical)
            && tracked.metadata.has_unit(metadata_sample)) {
            logical_sample_knobs_by_key.emplace(
                existing_identity_key(tracked.metadata, "logical-knob"),
                tracked);
        } else if (tracked.metadata.has_unit(metadata_input)
            && tracked.metadata.has_unit(metadata_logical)
            && tracked.metadata.has_unit(metadata_event)) {
            logical_event_inputs_by_key.emplace(
                existing_identity_key(tracked.metadata, "logical-event-input"),
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

    std::unordered_map<std::string, LaneId> logical_sample_knob_lanes;
    for (auto const &port : desired_ports) {
        if (port.port.port_kind != PortKind::sample || port.port.concrete_member_ordinal.has_value()) {
            continue;
        }
        auto const state_key = graph_input_port_key(port.port);
        auto const state = logical_sample_knob_states_by_key.find(state_key);
        if (state == logical_sample_knob_states_by_key.end()
            || state->second != LogicalSampleKnobState::timeline_lane) {
            continue;
        }

        auto const key = logical_knob_key(port);
        LaneId lane {};
        if (auto const it = logical_sample_knobs_by_key.find(key);
            it != logical_sample_knobs_by_key.end()) {
            lane = it->second.lane;
        } else {
            lane = lane_ids.next();
            if (batch != nullptr) {
                auto metadata = graph_input_metadata(
                    port,
                    true,
                    true,
                    false,
                    true,
                    false);
                auto const current_value =
                    live_input_value_or_locked(port.port.logical_node_id, port.port.port_ordinal, Sample{0.0f});
                batch->upserts.push_back(TimelineLaneUpsert{
                    .lane = lane,
                    .make_node = [current_value] {
                        return TypeErasedLaneNode(KnobLaneNode{ .value = current_value });
                    },
                    .metadata = std::move(metadata),
                });
            }
        }
        used_lane_ids.insert(lane.value);
        logical_sample_knob_lanes.emplace(key, lane);
        result.logical_sample_knobs.push_back(GraphInputLaneBinding{
            .port = port.port,
            .knob_lane = lane,
        });
    }

    for (auto const &port : desired_ports) {
        if (port.port.port_kind != PortKind::sample) {
            continue;
        }
        if (!port.port.concrete_member_ordinal.has_value()
            && has_concrete_descriptor_for_port(desired_ports, port)) {
            continue;
        }

        auto const logical_key = logical_knob_key(DesiredGraphInputPort{
            .instance_id = port.instance_id,
            .module_instance_id = port.module_instance_id,
            .port = GraphInputPortDescriptor{
                .logical_node_id = port.port.logical_node_id,
                .concrete_member_ordinal = std::nullopt,
                .port_kind = port.port.port_kind,
                .port_ordinal = port.port.port_ordinal,
                .port_name = port.port.port_name,
                .port_type = port.port.port_type,
            },
        });
        std::optional<LaneId> logical_knob;
        if (auto const it = logical_sample_knob_lanes.find(logical_key);
            it != logical_sample_knob_lanes.end()) {
            logical_knob = it->second;
        }

        ConcreteSampleInputState state =
            logical_knob.has_value()
                ? ConcreteSampleInputState::logical_follow
                : ConcreteSampleInputState::disconnected;
        if (!port.port.concrete_member_ordinal.has_value()) {
            state = ConcreteSampleInputState::logical_follow;
        }
        if (auto const it =
                concrete_sample_input_states_by_key.find(graph_input_port_key(port.port));
            it != concrete_sample_input_states_by_key.end()) {
            state = it->second;
        }
        if (state != ConcreteSampleInputState::timeline_lane) {
            continue;
        }

        auto const sample_key = sample_input_key(port);
        LaneId graph_input_lane {};
        if (auto const it = sample_inputs_by_key.find(sample_key);
            it != sample_inputs_by_key.end()) {
            graph_input_lane = it->second.lane;
        } else {
            graph_input_lane = lane_ids.next();
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
                batch->upserts.push_back(TimelineLaneUpsert{
                    .lane = graph_input_lane,
                    .make_node = [default_value] {
                        return make_sample_input_node(default_value);
                    },
                    .metadata = std::move(metadata),
                });
            }
        }
        used_lane_ids.insert(graph_input_lane.value);

        result.sample_inputs.push_back(GraphInputLaneBinding{
            .port = port.port,
            .graph_input_lane = graph_input_lane,
            .logical_knob_lane = logical_knob,
        });
    }

    auto ensure_logical_event_lane = [&](DesiredGraphInputPort const &port) {
        auto logical_port = port;
        logical_port.port.concrete_member_ordinal = std::nullopt;
        auto const key = logical_event_input_key(logical_port);
        if (auto const it = logical_event_inputs_by_key.find(key);
            it != logical_event_inputs_by_key.end()) {
            used_lane_ids.insert(it->second.lane.value);
            return it->second.lane;
        }

        auto const lane = lane_ids.next();
        if (batch != nullptr) {
            auto metadata = graph_input_metadata(
                logical_port,
                false,
                true,
                false,
                false,
                true);
            batch->upserts.push_back(TimelineLaneUpsert{
                .lane = lane,
                .make_node = [] {
                    return TypeErasedLaneNode(GraphEventInputLaneNode{});
                },
                .metadata = std::move(metadata),
            });
        }
        logical_event_inputs_by_key.emplace(
            key,
            ExistingTrackedLane{
                .lane = lane,
                .metadata = graph_input_metadata(
                    logical_port,
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
        if (!port.port.concrete_member_ordinal.has_value()
            && has_concrete_descriptor_for_port(desired_ports, port)) {
            continue;
        }

        ConcreteEventInputState state =
            port.default_connected
                ? ConcreteEventInputState::disconnected
                : ConcreteEventInputState::logical_follow;
        if (auto const it =
                concrete_event_input_states_by_key.find(graph_input_port_key(port.port));
            it != concrete_event_input_states_by_key.end()) {
            state = it->second;
        }

        LaneId graph_input_lane {};
        if (state == ConcreteEventInputState::logical_follow) {
            graph_input_lane = ensure_logical_event_lane(port);
        } else if (state == ConcreteEventInputState::timeline_lane) {
            auto const key = event_input_key(port);
            if (auto const it = event_inputs_by_key.find(key);
                it != event_inputs_by_key.end()) {
                graph_input_lane = it->second.lane;
            } else {
                graph_input_lane = lane_ids.next();
                if (batch != nullptr) {
                    auto metadata = graph_input_metadata(
                        port,
                        false,
                        false,
                        true,
                        false,
                        true);
                    batch->upserts.push_back(TimelineLaneUpsert{
                        .lane = graph_input_lane,
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
            if (!tracked.metadata.has_unit(metadata_graph_input)) {
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
    size_t input_ordinal)
{
    std::vector<GraphInputPortDescriptor> ports{
        sample_input_descriptor(node_id, std::nullopt, input_ordinal),
    };
    if (member_ordinal.has_value()) {
        ports.push_back(sample_input_descriptor(node_id, member_ordinal, input_ordinal));
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
                tracked.metadata = upsert.metadata;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            tracked_lanes.push_back(ExistingTrackedLane{
                .lane = upsert.lane,
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
    apply_tracked_batch_locked(batch);
    pending_timeline_batches.push_back(batch);
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

void GraphInputLanes::handle_iv_module_instance_builders_changed(
    IvModuleInstanceBuildersChanged const &diff,
    IvModuleInstanceBuildersAckBuilder *ack_builder)
{
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        for (auto const &created : diff.created) {
            if (created.instance == nullptr) {
                continue;
            }
            desired_ports_by_instance_id[created.instance->instance_id] =
                graph_input_port_descriptors_for(*created.instance);
        }
        for (auto const &updated : diff.updated) {
            if (updated.instance == nullptr) {
                continue;
            }
            desired_ports_by_instance_id[updated.instance->instance_id] =
                graph_input_port_descriptors_for(*updated.instance);
        }
        for (auto const &deleted_instance_id : diff.deleted_instance_ids) {
            desired_ports_by_instance_id.erase(deleted_instance_id);
        }
        refresh_desired_ports_locked();
        (void)reconcile_ports_locked(&batch);
        queue_timeline_batch_locked(batch);
    }

    GraphInputLanesDspTaskDependenciesChanged dsp_dependencies;
    dsp_dependencies.deleted_instance_ids = diff.deleted_instance_ids;

    for (auto const &created : diff.created) {
        if (created.instance == nullptr || created.builder == nullptr) {
            continue;
        }
        auto completion = complete_builder(created.instance->instance_id, *created.builder);
        if (ack_builder != nullptr) {
            ack_builder->set_prerequisite_lanes(
                created.instance->instance_id,
                completion.prerequisite_lanes);
        }
        dsp_dependencies.created_or_updated.push_back(GraphInputLanesDspTaskDependencies {
            .instance_id = created.instance->instance_id,
            .prerequisite_lanes = std::move(completion.prerequisite_lanes),
        });
    }
    for (auto const &updated : diff.updated) {
        if (updated.instance == nullptr || updated.builder == nullptr) {
            continue;
        }
        auto completion = complete_builder(updated.instance->instance_id, *updated.builder);
        if (ack_builder != nullptr) {
            ack_builder->set_prerequisite_lanes(
                updated.instance->instance_id,
                completion.prerequisite_lanes);
        }
        dsp_dependencies.created_or_updated.push_back(GraphInputLanesDspTaskDependencies {
            .instance_id = updated.instance->instance_id,
            .prerequisite_lanes = std::move(completion.prerequisite_lanes),
        });
    }

    if (!dsp_dependencies.created_or_updated.empty() || !dsp_dependencies.deleted_instance_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_graph_input_lanes_dsp_task_dependencies_changed_event,
            dsp_dependencies);
    }
}

std::vector<IvModuleSourceIntrospectionLiveInputSnapshot>
GraphInputLanes::collect_live_input_snapshots(
    std::vector<IvModuleSourceIntrospectionLiveInputSnapshotRequest> const &requests)
{
    std::scoped_lock lock(mutex);
    std::vector<IvModuleSourceIntrospectionLiveInputSnapshot> snapshots;
    snapshots.reserve(requests.size());
    for (auto const &request : requests) {
        snapshots.push_back(IvModuleSourceIntrospectionLiveInputSnapshot{
            .logical_node_id = request.logical_node_id,
            .member_ordinal = request.member_ordinal,
            .input_ordinal = request.input_ordinal,
            .current_value = request.member_ordinal.has_value()
                ? live_input_value_or_locked(
                    request.logical_node_id,
                    *request.member_ordinal,
                    request.input_ordinal,
                    request.fallback)
                : live_input_value_or_locked(
                    request.logical_node_id,
                    request.input_ordinal,
                    request.fallback),
            .has_concrete_override = request.member_ordinal.has_value()
                ? concrete_live_input_overrides.contains(
                    concrete_override_key(
                        request.logical_node_id,
                        *request.member_ordinal,
                        request.input_ordinal))
                : false,
        });
    }
    return snapshots;
}

GraphInputLaneBindings GraphInputLanes::query_graph_input_lane_bindings(
    ProjectGraphInputLaneBindingsRequest const &request)
{
    std::scoped_lock lock(mutex);
    GraphInputLaneBindings bindings;

    auto find_lane = [&](std::function<bool(ExistingTrackedLane const &)> const &matches) -> LaneId {
        for (auto const &tracked : tracked_lanes) {
            if (matches(tracked)) {
                return tracked.lane;
            }
        }
        return LaneId{};
    };

    for (auto const &port : request.ports) {
        if (port.port_kind == PortKind::sample && !port.concrete_member_ordinal.has_value()) {
            auto logical_lane = find_lane([&](ExistingTrackedLane const &tracked) {
                return tracked.metadata.has_unit(metadata_knob)
                    && tracked.metadata.has_unit(metadata_logical)
                    && tracked.metadata.has_unit(metadata_sample)
                    && tracked.metadata.int_value(metadata_logical_node_id) == hash_string(port.logical_node_id)
                    && tracked.metadata.int_value(metadata_port_kind) == 0
                    && tracked.metadata.int_value(metadata_port_ordinal) == static_cast<int>(port.port_ordinal);
            });
            if (logical_lane) {
                bindings.logical_sample_knobs.push_back(GraphInputLaneBinding{
                    .port = port,
                    .knob_lane = logical_lane,
                });
            }
        }
    }

    for (auto const &port : request.ports) {
        if (port.port_kind == PortKind::sample) {
            auto concrete_lane = find_lane([&](ExistingTrackedLane const &tracked) {
                return tracked.metadata.has_unit(metadata_knob)
                    && tracked.metadata.has_unit(metadata_concrete)
                    && tracked.metadata.has_unit(metadata_sample)
                    && tracked.metadata.int_value(metadata_logical_node_id) == hash_string(port.logical_node_id)
                    && tracked.metadata.int_value(metadata_port_kind) == 0
                    && tracked.metadata.int_value(metadata_port_ordinal) == static_cast<int>(port.port_ordinal)
                    && tracked.metadata.int_value(metadata_member_ordinal)
                        == (port.concrete_member_ordinal.has_value()
                            ? std::optional<int>(static_cast<int>(*port.concrete_member_ordinal))
                            : std::nullopt);
            });
            auto graph_input_lane = find_lane([&](ExistingTrackedLane const &tracked) {
                return tracked.metadata.has_unit(metadata_input)
                    && tracked.metadata.has_unit(metadata_sample)
                    && tracked.metadata.int_value(metadata_logical_node_id) == hash_string(port.logical_node_id)
                    && tracked.metadata.int_value(metadata_port_kind) == 0
                    && tracked.metadata.int_value(metadata_port_ordinal) == static_cast<int>(port.port_ordinal)
                    && tracked.metadata.int_value(metadata_member_ordinal)
                        == (port.concrete_member_ordinal.has_value()
                            ? std::optional<int>(static_cast<int>(*port.concrete_member_ordinal))
                            : std::nullopt);
            });
            if (concrete_lane || graph_input_lane) {
                std::optional<LaneId> logical_knob;
                for (auto const &binding : bindings.logical_sample_knobs) {
                    if (binding.port.logical_node_id == port.logical_node_id
                        && binding.port.port_kind == port.port_kind
                        && binding.port.port_ordinal == port.port_ordinal) {
                        logical_knob = binding.knob_lane;
                        break;
                    }
                }
                bindings.sample_inputs.push_back(GraphInputLaneBinding{
                    .port = port,
                    .knob_lane = concrete_lane,
                    .graph_input_lane = graph_input_lane,
                    .logical_knob_lane = logical_knob,
                });
            }
            continue;
        }

        auto graph_input_lane = find_lane([&](ExistingTrackedLane const &tracked) {
            return tracked.metadata.has_unit(metadata_input)
                && tracked.metadata.has_unit(metadata_event)
                && tracked.metadata.int_value(metadata_logical_node_id) == hash_string(port.logical_node_id)
                && tracked.metadata.int_value(metadata_port_kind) == 1
                && tracked.metadata.int_value(metadata_port_ordinal) == static_cast<int>(port.port_ordinal)
                && tracked.metadata.int_value(metadata_member_ordinal)
                    == (port.concrete_member_ordinal.has_value()
                        ? std::optional<int>(static_cast<int>(*port.concrete_member_ordinal))
                        : std::nullopt);
        });
        if (graph_input_lane) {
            bindings.event_inputs.push_back(GraphInputLaneBinding{
                .port = port,
                .graph_input_lane = graph_input_lane,
            });
        }
    }

    return bindings;
}

void GraphInputLanes::set_sample_input_value(
    ProjectSetSampleInputValueRequest const &request)
{
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        if (request.member_ordinal.has_value()) {
            auto const key = concrete_key(request.node_id, *request.member_ordinal);
            auto const port_key = graph_input_port_key(sample_input_descriptor(
                request.node_id,
                request.member_ordinal,
                request.input_ordinal));
            concrete_live_input_overrides.insert(
                concrete_override_key(request.node_id, *request.member_ordinal, request.input_ordinal));
            concrete_sample_input_states_by_key[port_key] =
                ConcreteSampleInputState::overridden;
            ensure_live_input_value_locked(key, request.input_ordinal)
                .store(request.value.value, std::memory_order_relaxed);
            auto &slots = ensure_live_input_slots_locked(key, request.input_ordinal);
            for (auto *slot : slots) {
                if (slot) {
                    *slot = request.value;
                }
            }
        } else {
            auto const port_key = graph_input_port_key(sample_input_descriptor(
                request.node_id,
                std::nullopt,
                request.input_ordinal));
            logical_sample_knob_states_by_key[port_key] =
                LogicalSampleKnobState::overridden;
            ensure_live_input_value_locked(request.node_id, request.input_ordinal)
                .store(request.value.value, std::memory_order_relaxed);
            if (auto it = live_inputs.find(std::string(request.node_id));
                it != live_inputs.end() && it->second.size() > request.input_ordinal) {
                for (auto *slot : it->second[request.input_ordinal]) {
                    if (slot) {
                        *slot = request.value;
                    }
                }
            }
            std::string const prefix = concrete_key_prefix(request.node_id);
            for (auto &[key, slots_by_input] : live_inputs) {
                if (!key.starts_with(prefix) || slots_by_input.size() <= request.input_ordinal) {
                    continue;
                }
                auto const input_override_key = key + "\x1finput:" + std::to_string(request.input_ordinal);
                if (concrete_live_input_overrides.contains(input_override_key)) {
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
        (void)reconcile_ports_locked(&batch);
        queue_timeline_batch_locked(batch);
    }
}

void GraphInputLanes::set_sample_input_state(
    ProjectSetSampleInputStateRequest const &request)
{
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        if (request.member_ordinal.has_value()) {
            auto const port_key = graph_input_port_key(sample_input_descriptor(
                request.node_id,
                request.member_ordinal,
                request.input_ordinal));
            auto const override_key = concrete_override_key(
                request.node_id,
                *request.member_ordinal,
                request.input_ordinal);

            switch (request.state) {
            case ProjectSampleInputState::default_:
                concrete_live_input_overrides.erase(override_key);
                concrete_sample_input_states_by_key.erase(port_key);
                break;
            case ProjectSampleInputState::overridden:
                concrete_live_input_overrides.insert(override_key);
                concrete_sample_input_states_by_key[port_key] =
                    ConcreteSampleInputState::overridden;
                break;
            case ProjectSampleInputState::logical_follow:
                concrete_live_input_overrides.erase(override_key);
                concrete_sample_input_states_by_key[port_key] =
                    ConcreteSampleInputState::logical_follow;
                break;
            case ProjectSampleInputState::timeline_lane:
                concrete_live_input_overrides.erase(override_key);
                concrete_sample_input_states_by_key[port_key] =
                    ConcreteSampleInputState::timeline_lane;
                break;
            case ProjectSampleInputState::disconnected:
                concrete_live_input_overrides.erase(override_key);
                concrete_sample_input_states_by_key[port_key] =
                    ConcreteSampleInputState::disconnected;
                break;
            }

            mark_instances_requiring_rebuild_locked(
                request.node_id,
                request.member_ordinal,
                request.input_ordinal);
        } else {
            auto const port_key = graph_input_port_key(sample_input_descriptor(
                request.node_id,
                std::nullopt,
                request.input_ordinal));
            switch (request.state) {
            case ProjectSampleInputState::default_:
            case ProjectSampleInputState::overridden:
                logical_sample_knob_states_by_key[port_key] =
                    LogicalSampleKnobState::overridden;
                break;
            case ProjectSampleInputState::timeline_lane:
                logical_sample_knob_states_by_key[port_key] =
                    LogicalSampleKnobState::timeline_lane;
                break;
            case ProjectSampleInputState::logical_follow:
            case ProjectSampleInputState::disconnected:
                throw std::runtime_error(
                    "logical sample input state only supports overridden or timeline_lane");
            }
            mark_instances_requiring_rebuild_for_logical_sample_input_locked(
                request.node_id,
                request.input_ordinal);
        }
        (void)reconcile_ports_locked(&batch);
        queue_timeline_batch_locked(batch);
    }
}

void GraphInputLanes::set_event_input_state(
    ProjectSetEventInputStateRequest const &request)
{
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        auto const port_key = graph_input_port_key(GraphInputPortDescriptor{
            .logical_node_id = request.node_id,
            .concrete_member_ordinal = request.member_ordinal,
            .port_kind = PortKind::event,
            .port_ordinal = request.input_ordinal,
        });

        switch (request.state) {
        case ProjectEventInputState::default_:
            concrete_event_input_states_by_key.erase(port_key);
            break;
        case ProjectEventInputState::logical_follow:
            concrete_event_input_states_by_key[port_key] =
                ConcreteEventInputState::logical_follow;
            break;
        case ProjectEventInputState::timeline_lane:
            concrete_event_input_states_by_key[port_key] =
                ConcreteEventInputState::timeline_lane;
            break;
        case ProjectEventInputState::disconnected:
            concrete_event_input_states_by_key[port_key] =
                ConcreteEventInputState::disconnected;
            break;
        }

        mark_instances_requiring_rebuild_locked(
            request.node_id,
            request.member_ordinal,
            request.input_ordinal);
        (void)reconcile_ports_locked(&batch);
        queue_timeline_batch_locked(batch);
    }
}

GraphInputLaneBindings GraphInputLanes::graph_input_lane_bindings(
    ProjectGraphInputLaneBindingsRequest const &request)
{
    return query_graph_input_lane_bindings(request);
}

void GraphInputLanes::handle_task_runner_pass_finished(
    TaskRunnerPassFinished const &)
{
    std::vector<std::string> instance_ids;
    std::vector<TimelineLaneBatchUpdate> timeline_batches;
    {
        std::scoped_lock lock(mutex);
        instance_ids.assign(
            pending_rebuild_instance_ids.begin(),
            pending_rebuild_instance_ids.end());
        pending_rebuild_instance_ids.clear();
        timeline_batches = take_pending_timeline_batches_locked();
    }
    for (auto const &batch : timeline_batches) {
        apply_timeline_batch(batch);
    }
    if (instance_ids.empty()) {
        return;
    }
    std::ranges::sort(instance_ids);
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_rebuild_requested_event,
        GraphInputLanesRebuildRequested{
            .instance_ids = std::move(instance_ids),
        });
}

GraphInputLanes::BuilderCompletionDiff GraphInputLanes::complete_builder(
    std::string const &instance_id,
    GraphBuilder &builder)
{
    BuilderCompletionDiff diff;
    auto const logical_inputs = builder.logical_inputs();
    if (logical_inputs.sample.empty() && logical_inputs.event.empty()) {
        return diff;
    }
    auto const module_instance_id = module_instance_numeric_id(instance_id);

    {
        std::scoped_lock lock(mutex);
        std::erase_if(desired_ports, [&](DesiredGraphInputPort const &port) {
            return port.instance_id == instance_id;
        });

        std::unordered_set<std::string> appended;
        auto append_port = [&](GraphInputPortDescriptor descriptor) {
            DesiredGraphInputPort desired{
                .instance_id = instance_id,
                .module_instance_id = module_instance_id,
                .port = std::move(descriptor),
            };
            auto const key = desired_port_key(desired);
            if (!appended.insert(key).second) {
                return;
            }
            desired_ports.push_back(std::move(desired));
        };

        for (auto const &input : logical_inputs.sample) {
            auto logical = sample_port_descriptor(input, std::nullopt);
            auto concrete = sample_port_descriptor(input, input.member_ordinal);
            sample_input_default_values[sample_default_value_key(instance_id, logical)] =
                input.config.default_value;
            sample_input_default_values[sample_default_value_key(instance_id, concrete)] =
                input.config.default_value;
            append_port(std::move(logical));
            append_port(std::move(concrete));
        }
        for (auto const &input : logical_inputs.event) {
            append_port(event_port_descriptor(input, std::nullopt));
            append_port(event_port_descriptor(input, input.member_ordinal));
        }

        (void)reconcile_ports_locked(&diff.timeline_batch);
        queue_timeline_batch_locked(diff.timeline_batch);
    }

    std::unordered_map<std::string, size_t> sample_control_node_indices;
    for (auto const &input : logical_inputs.sample) {
        auto const concrete_port = sample_port_descriptor(input, input.member_ordinal);
        auto const state_key = instance_port_state_key("", concrete_port);
        auto const logical_default_value = live_input_value_or_locked(
            input.logical_node_id,
            input.target.port,
            input.config.default_value);
        (void)ensure_live_input_value_initialized_locked(
            input.logical_node_id,
            input.target.port,
            input.config.default_value);
        (void)ensure_live_input_value_initialized_locked(
            concrete_key(input.logical_node_id, input.member_ordinal),
            input.target.port,
            logical_default_value);

        ConcreteSampleInputState state =
            input.has_existing_connection
                ? ConcreteSampleInputState::disconnected
                : ConcreteSampleInputState::logical_follow;
        if (auto const it = concrete_sample_input_states_by_key.find(state_key);
            it != concrete_sample_input_states_by_key.end()) {
            state = it->second;
        }

        SamplePortRef source;
        bool have_source = false;
        LaneId prerequisite_lane {};

        if (state == ConcreteSampleInputState::timeline_lane) {
            auto const bindings = sample_input_bindings(
                input.logical_node_id,
                input.member_ordinal,
                input.target.port);
            if (bindings.sample_inputs.empty()) {
                throw std::runtime_error("graph input lane completion could not resolve sample input lane");
            }
            prerequisite_lane = bindings.sample_inputs.front().graph_input_lane;
            auto const identity = LaneInputValue::nominal_identity(prerequisite_lane);
            if (auto const existing = sample_control_node_indices.find(identity);
                existing != sample_control_node_indices.end()) {
                source = SamplePortRef(builder, existing->second, 0);
            } else {
                source = builder.node<LaneInputValue>(prerequisite_lane);
                sample_control_node_indices.emplace(identity, source.node_index);
            }
            have_source = true;
            builder.mark_runtime_filled_sample_input(input.target);
        } else if (state == ConcreteSampleInputState::overridden) {
            auto const key = concrete_key(input.logical_node_id, input.member_ordinal);
            source = builder.node<ValueSource>(
                live_input_value_ptr_or_locked(key, input.target.port));
            have_source = true;
        } else if (state == ConcreteSampleInputState::logical_follow) {
            auto const logical_state_key = graph_input_port_key(
                sample_port_descriptor(input, std::nullopt));
            auto const logical_state_it =
                logical_sample_knob_states_by_key.find(logical_state_key);
            auto const logical_state =
                logical_state_it != logical_sample_knob_states_by_key.end()
                    ? logical_state_it->second
                    : LogicalSampleKnobState::overridden;

            if (logical_state == LogicalSampleKnobState::timeline_lane) {
                auto const bindings = sample_input_bindings(
                    input.logical_node_id,
                    std::nullopt,
                    input.target.port);
                if (bindings.logical_sample_knobs.empty()) {
                    throw std::runtime_error(
                        "logical sample knob timeline lane could not be resolved");
                }
                prerequisite_lane = bindings.logical_sample_knobs.front().knob_lane;
                auto const identity = LaneInputValue::nominal_identity(prerequisite_lane);
                if (auto const existing = sample_control_node_indices.find(identity);
                    existing != sample_control_node_indices.end()) {
                    source = SamplePortRef(builder, existing->second, 0);
                } else {
                    source = builder.node<LaneInputValue>(prerequisite_lane);
                    sample_control_node_indices.emplace(identity, source.node_index);
                }
            } else {
                source = builder.node<ValueSource>(
                    live_input_value_ptr_or_locked(input.logical_node_id, input.target.port));
            }
            have_source = true;
        } else if (!input.has_existing_connection) {
            source = builder.node<Constant>(input.config.default_value);
            have_source = true;
        }

        if (have_source) {
            builder.connect_sample_input(input.target, source);
        }

        diff.sample_inputs.push_back(CompletedSampleInput{
            .input = GraphBuilder::VacantSampleInput{
                .target = input.target,
                .logical_node_id = input.logical_node_id,
                .member_ordinal = input.member_ordinal,
                .config = input.config,
            },
            .lane = prerequisite_lane,
        });
        if (prerequisite_lane) {
            diff.prerequisite_lanes.push_back(prerequisite_lane);
        }
    }

    for (auto const &input : logical_inputs.event) {
        auto const concrete_port = event_port_descriptor(input, input.member_ordinal);
        auto const state_key = instance_port_state_key("", concrete_port);
        ConcreteEventInputState state =
            input.has_existing_connection
                ? ConcreteEventInputState::disconnected
                : ConcreteEventInputState::logical_follow;
        if (auto const it = concrete_event_input_states_by_key.find(state_key);
            it != concrete_event_input_states_by_key.end()) {
            state = it->second;
        }

        if (state == ConcreteEventInputState::default_) {
            state =
                input.has_existing_connection
                    ? ConcreteEventInputState::disconnected
                    : ConcreteEventInputState::logical_follow;
        }

        if (state == ConcreteEventInputState::disconnected) {
            continue;
        }

        auto requested_port =
            state == ConcreteEventInputState::timeline_lane
                ? event_port_descriptor(input, input.member_ordinal)
                : event_port_descriptor(input, std::nullopt);
        auto const bindings = query_graph_input_lane_bindings(ProjectGraphInputLaneBindingsRequest{
            .ports = { requested_port },
        });
        if (bindings.event_inputs.empty()) {
            throw std::runtime_error("graph input lane completion could not resolve event input lane");
        }
        auto source = builder.node<EventConcatenation>(0, input.config.type).event_port(0);
        builder.connect_event_input(input.target, source);
        builder.mark_runtime_filled_event_input(input.target);
        diff.event_inputs.push_back(CompletedEventInput{
            .input = GraphBuilder::VacantEventInput{
                .target = input.target,
                .logical_node_id = input.logical_node_id,
                .member_ordinal = input.member_ordinal,
                .config = input.config,
            },
            .lane = bindings.event_inputs.front().graph_input_lane,
        });
        diff.prerequisite_lanes.push_back(bindings.event_inputs.front().graph_input_lane);
    }

    std::ranges::sort(diff.prerequisite_lanes, {}, &LaneId::value);
    diff.prerequisite_lanes.erase(
        std::unique(diff.prerequisite_lanes.begin(), diff.prerequisite_lanes.end()),
        diff.prerequisite_lanes.end());

    return diff;
}
} // namespace iv
