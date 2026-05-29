#include "runtime/graph_input_lanes.h"

#include "basic_nodes/buffers.h"
#include "basic_nodes/routing.h"
#include "basic_lane_nodes/controls.h"

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
    std::span<LogicalPortInfo const> logical_ports)
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

std::string concrete_knob_key(GraphInputLanes::DesiredGraphInputPort const &port)
{
    auto const logical_node_id = local_hash_string(port.port.logical_node_id);
    auto const concrete_node_id = local_hash_string(
        port.port.logical_node_id
        + "\x1f"
        + "member:"
        + (port.port.concrete_member_ordinal.has_value()
            ? std::to_string(*port.port.concrete_member_ordinal)
            : std::string("logical")));

    return "concrete-knob:instance:" + std::to_string(port.module_instance_id)
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
    GraphBuilder::VacantSampleInput const &input,
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
    GraphBuilder::VacantEventInput const &input,
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
    IvModuleInstanceBuilder const &instance_builder)
{
    std::vector<DesiredGraphInputPort> ports;
    auto const module_instance_id = module_instance_numeric_id(instance_builder.instance.instance_id);
    for (auto const &node : instance_builder.instance.introspection.logical_nodes) {
        append_graph_input_port_descriptors(
            ports,
            instance_builder.instance.instance_id,
            module_instance_id,
            node.id,
            std::nullopt,
            PortKind::sample,
            node.sample_inputs);
        append_graph_input_port_descriptors(
            ports,
            instance_builder.instance.instance_id,
            module_instance_id,
            node.id,
            std::nullopt,
            PortKind::event,
            node.event_inputs);
        for (auto const &member : node.members) {
            append_graph_input_port_descriptors(
                ports,
                instance_builder.instance.instance_id,
                module_instance_id,
                node.id,
                member.ordinal,
                PortKind::sample,
                member.sample_inputs);
            append_graph_input_port_descriptors(
                ports,
                instance_builder.instance.instance_id,
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

Sample& GraphInputLanes::ensure_live_input_value_locked(
    std::string_view key,
    size_t input_ordinal)
{
    auto &values = live_input_values[std::string(key)];
    if (values.size() <= input_ordinal) {
        values.resize(input_ordinal + 1);
    }
    return values[input_ordinal];
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
    return it->second[input_ordinal];
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
        if (it != live_input_values.end() && it->second.size() > input_ordinal) {
            return it->second[input_ordinal];
        }
    }
    return live_input_value_or_locked(logical_node_id, input_ordinal, fallback);
}

void GraphInputLanes::refresh_desired_ports_locked()
{
    desired_ports.clear();
    for (auto const &[_, instance_builder] : builders_by_instance_id) {
        auto ports = graph_input_port_descriptors_for(instance_builder);
        desired_ports.insert(
            desired_ports.end(),
            std::make_move_iterator(ports.begin()),
            std::make_move_iterator(ports.end()));
    }
}

GraphInputLaneBindings GraphInputLanes::reconcile_ports_locked(TimelineLaneBatchUpdate *batch)
{
    GraphInputLaneBindings result;
    std::unordered_map<std::string, ExistingTrackedLane> logical_sample_knobs_by_key;
    std::unordered_map<std::string, ExistingTrackedLane> concrete_sample_knobs_by_key;
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
        } else if (tracked.metadata.has_unit(metadata_knob)
            && tracked.metadata.has_unit(metadata_concrete)
            && tracked.metadata.has_unit(metadata_sample)) {
            concrete_sample_knobs_by_key.emplace(
                existing_identity_key(tracked.metadata, "concrete-knob"),
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

        auto const concrete_key = concrete_knob_key(port);
        LaneId concrete_knob {};
        bool concrete_knob_created = false;
        if (auto const it = concrete_sample_knobs_by_key.find(concrete_key);
            it != concrete_sample_knobs_by_key.end()) {
            concrete_knob = it->second.lane;
        } else {
            concrete_knob = lane_ids.next();
            concrete_knob_created = true;
            if (batch != nullptr) {
                auto metadata = graph_input_metadata(
                    port,
                    true,
                    false,
                    true,
                    true,
                    false);
                auto const current_value =
                    port.port.concrete_member_ordinal.has_value()
                        ? live_input_value_or_locked(
                            port.port.logical_node_id,
                            *port.port.concrete_member_ordinal,
                            port.port.port_ordinal,
                            live_input_value_or_locked(
                                port.port.logical_node_id,
                                port.port.port_ordinal,
                                Sample{0.0f}))
                        : live_input_value_or_locked(
                            port.port.logical_node_id,
                            port.port.port_ordinal,
                            Sample{0.0f});
                batch->upserts.push_back(TimelineLaneUpsert{
                    .lane = concrete_knob,
                    .make_node = [current_value] {
                        return TypeErasedLaneNode(KnobLaneNode{ .value = current_value });
                    },
                    .metadata = std::move(metadata),
                });
            }
        }
        used_lane_ids.insert(concrete_knob.value);

        auto const sample_key = sample_input_key(port);
        LaneId graph_input_lane {};
        bool graph_input_created = false;
        if (auto const it = sample_inputs_by_key.find(sample_key);
            it != sample_inputs_by_key.end()) {
            graph_input_lane = it->second.lane;
        } else {
            graph_input_lane = lane_ids.next();
            graph_input_created = true;
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

        if (batch != nullptr) {
            if (logical_knob.has_value() && *logical_knob != concrete_knob) {
                batch->connections_to_add.push_back(LaneGraphConnection{
                    .source = *logical_knob,
                    .target = concrete_knob,
                    .input = realtime_sample_input(),
                });
            }
            batch->connections_to_add.push_back(LaneGraphConnection{
                .source = concrete_knob,
                .target = graph_input_lane,
                .input = realtime_sample_input(),
            });
            batch->hierarchy_additions.push_back(TimelineLaneHierarchyUpdate{
                .parent = concrete_knob,
                .child = graph_input_lane,
            });
            (void)concrete_knob_created;
            (void)graph_input_created;
        }

        result.sample_inputs.push_back(GraphInputLaneBinding{
            .port = port.port,
            .knob_lane = concrete_knob,
            .graph_input_lane = graph_input_lane,
            .logical_knob_lane = logical_knob,
        });
    }

    for (auto const &port : desired_ports) {
        if (port.port.port_kind != PortKind::event) {
            continue;
        }
        if (!port.port.concrete_member_ordinal.has_value()
            && has_concrete_descriptor_for_port(desired_ports, port)) {
            continue;
        }

        auto const key = event_input_key(port);
        LaneId graph_input_lane {};
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

void GraphInputLanes::apply_timeline_batch(TimelineLaneBatchUpdate const &batch)
{
    GraphInputLanesAckBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_timeline_batch_requested_event,
        batch,
        builder);
    builder.build();

    std::scoped_lock lock(mutex);
    apply_tracked_batch_locked(batch);
}

void GraphInputLanes::set_realtime_lane_ref_factory(
    std::function<RealtimeLaneRef(LaneId)> factory)
{
    std::scoped_lock lock(mutex);
    realtime_lane_ref_for_lane = std::move(factory);
}

void GraphInputLanes::handle_iv_module_instance_builders_changed(
    IvModuleInstanceBuildersChanged const &diff)
{
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        for (auto const &created : diff.created) {
            builders_by_instance_id[created.instance.instance_id] = created;
        }
        for (auto const &updated : diff.updated) {
            builders_by_instance_id[updated.instance.instance_id] = updated;
        }
        for (auto const &deleted_instance_id : diff.deleted_instance_ids) {
            builders_by_instance_id.erase(deleted_instance_id);
        }
        refresh_desired_ports_locked();
        (void)reconcile_ports_locked(&batch);
    }

    if (batch_has_changes(batch)) {
        apply_timeline_batch(batch);
    }

    for (auto created : diff.created) {
        (void)complete_builder(created.instance.instance_id, created.builder);
        std::scoped_lock lock(mutex);
        builders_by_instance_id[created.instance.instance_id] = std::move(created);
    }
    for (auto updated : diff.updated) {
        (void)complete_builder(updated.instance.instance_id, updated.builder);
        std::scoped_lock lock(mutex);
        builders_by_instance_id[updated.instance.instance_id] = std::move(updated);
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
    auto const bindings = sample_input_bindings(
        request.node_id,
        request.member_ordinal,
        request.input_ordinal);
    auto const knob_lane =
        request.member_ordinal.has_value()
            ? (!bindings.sample_inputs.empty() ? bindings.sample_inputs.front().knob_lane : LaneId{})
            : (!bindings.logical_sample_knobs.empty() ? bindings.logical_sample_knobs.front().knob_lane : LaneId{});
    auto const inherited_lane =
        request.member_ordinal.has_value() && !bindings.logical_sample_knobs.empty()
            ? bindings.logical_sample_knobs.front().knob_lane
            : LaneId{};

    std::optional<LaneMetadata> knob_metadata;
    {
        std::scoped_lock lock(mutex);
        if (request.member_ordinal.has_value()) {
            auto const key = concrete_key(request.node_id, *request.member_ordinal);
            concrete_live_input_overrides.insert(
                concrete_override_key(request.node_id, *request.member_ordinal, request.input_ordinal));
            ensure_live_input_value_locked(key, request.input_ordinal) = request.value;
            auto &slots = ensure_live_input_slots_locked(key, request.input_ordinal);
            for (auto *slot : slots) {
                if (slot) {
                    *slot = request.value;
                }
            }
        } else {
            ensure_live_input_value_locked(request.node_id, request.input_ordinal) = request.value;
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
                ensure_live_input_value_locked(key, request.input_ordinal) = request.value;
            }
        }
        if (knob_lane) {
            knob_metadata = tracked_lane_metadata_locked(knob_lane);
        }
    }
    TimelineLaneBatchUpdate batch;
    if (knob_lane && knob_metadata.has_value()) {
        batch.upserts.push_back(TimelineLaneUpsert{
            .lane = knob_lane,
            .make_node = [value = request.value] {
                return TypeErasedLaneNode(KnobLaneNode{ .value = value });
            },
            .metadata = *knob_metadata,
        });
    }
    if (inherited_lane && knob_lane) {
        batch.connections_to_remove.push_back(LaneGraphConnection{
            .source = inherited_lane,
            .target = knob_lane,
            .input = realtime_sample_input(),
        });
    }
    if (batch_has_changes(batch)) {
        apply_timeline_batch(batch);
    }
}

void GraphInputLanes::clear_sample_input_value_override(
    ProjectClearSampleInputValueOverrideRequest const &request)
{
    auto const bindings = sample_input_bindings(
        request.node_id,
        request.member_ordinal,
        request.input_ordinal);
    auto const knob_lane =
        !bindings.sample_inputs.empty() ? bindings.sample_inputs.front().knob_lane : LaneId{};
    auto const inherited_lane =
        !bindings.logical_sample_knobs.empty() ? bindings.logical_sample_knobs.front().knob_lane : LaneId{};

    Sample logical_value = Sample{0.0f};
    std::optional<LaneMetadata> knob_metadata;
    {
        std::scoped_lock lock(mutex);
        concrete_live_input_overrides.erase(
            concrete_override_key(request.node_id, request.member_ordinal, request.input_ordinal));

        auto const key = concrete_key(request.node_id, request.member_ordinal);
        logical_value = live_input_value_or_locked(request.node_id, request.input_ordinal, Sample{0.0f});
        ensure_live_input_value_locked(key, request.input_ordinal) = logical_value;
        auto &slots = ensure_live_input_slots_locked(key, request.input_ordinal);
        for (auto *slot : slots) {
            if (slot) {
                *slot = logical_value;
            }
        }
        if (knob_lane) {
            knob_metadata = tracked_lane_metadata_locked(knob_lane);
        }
    }
    TimelineLaneBatchUpdate batch;
    if (knob_lane && knob_metadata.has_value()) {
        batch.upserts.push_back(TimelineLaneUpsert{
            .lane = knob_lane,
            .make_node = [logical_value] {
                return TypeErasedLaneNode(KnobLaneNode{ .value = logical_value });
            },
            .metadata = *knob_metadata,
        });
    }
    if (inherited_lane && knob_lane) {
        batch.connections_to_add.push_back(LaneGraphConnection{
            .source = inherited_lane,
            .target = knob_lane,
            .input = realtime_sample_input(),
        });
    }
    if (batch_has_changes(batch)) {
        apply_timeline_batch(batch);
    }
}

GraphInputLanes::BuilderCompletionDiff GraphInputLanes::complete_builder(
    std::string const &instance_id,
    GraphBuilder &builder)
{
    BuilderCompletionDiff diff;
    auto const vacant = builder.vacant_inputs();
    if (vacant.sample.empty() && vacant.event.empty()) {
        return diff;
    }
    auto const module_instance_id = module_instance_numeric_id(instance_id);
    std::function<RealtimeLaneRef(LaneId)> ref_factory;

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

        for (auto const &input : vacant.sample) {
            auto logical = sample_port_descriptor(input, std::nullopt);
            auto concrete = sample_port_descriptor(input, input.member_ordinal);
            sample_input_default_values[sample_default_value_key(instance_id, logical)] =
                input.config.default_value;
            sample_input_default_values[sample_default_value_key(instance_id, concrete)] =
                input.config.default_value;
            append_port(std::move(logical));
            append_port(std::move(concrete));
        }
        for (auto const &input : vacant.event) {
            append_port(event_port_descriptor(input, std::nullopt));
            append_port(event_port_descriptor(input, input.member_ordinal));
        }

        (void)reconcile_ports_locked(&diff.timeline_batch);
        ref_factory = realtime_lane_ref_for_lane;
    }

    if (batch_has_changes(diff.timeline_batch)) {
        apply_timeline_batch(diff.timeline_batch);
    }

    if (!vacant.sample.empty() && !ref_factory) {
        throw std::runtime_error("graph input lanes realtime lane ref factory was not bound");
    }

    std::unordered_map<std::string, size_t> sample_control_node_indices;
    for (auto const &input : vacant.sample) {
        auto const bindings = sample_input_bindings(
            input.logical_node_id,
            input.member_ordinal,
            input.target.port);
        if (bindings.sample_inputs.empty()) {
            throw std::runtime_error("graph input lane completion could not resolve sample input lane");
        }
        auto const lane = bindings.sample_inputs.front().graph_input_lane;
        auto const lane_ref = ref_factory(lane);
        auto const identity = LaneInputValue::nominal_identity(lane_ref);
        SamplePortRef source;
        if (auto const existing = sample_control_node_indices.find(identity);
            existing != sample_control_node_indices.end()) {
            source = SamplePortRef(builder, existing->second, 0);
        } else {
            source = builder.node<LaneInputValue>(lane_ref);
            sample_control_node_indices.emplace(identity, source.node_index);
        }
        builder.connect_sample_input(input.target, source);
        builder.mark_runtime_filled_sample_input(input.target);
        diff.sample_inputs.push_back(CompletedSampleInput{
            .input = input,
            .lane = lane,
        });
    }

    for (auto const &input : vacant.event) {
        auto const bindings = query_graph_input_lane_bindings(ProjectGraphInputLaneBindingsRequest{
            .ports = { event_port_descriptor(input, input.member_ordinal) },
        });
        if (bindings.event_inputs.empty()) {
            throw std::runtime_error("graph input lane completion could not resolve event input lane");
        }
        auto source = builder.node<EventConcatenation>(0, input.config.type).event_port(0);
        builder.connect_event_input(input.target, source);
        builder.mark_runtime_filled_event_input(input.target);
        diff.event_inputs.push_back(CompletedEventInput{
            .input = input,
            .lane = bindings.event_inputs.front().graph_input_lane,
        });
    }

    return diff;
}
} // namespace iv
