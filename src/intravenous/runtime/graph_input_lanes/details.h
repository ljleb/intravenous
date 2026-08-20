#pragma once

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/basic_nodes/buffers.h>

namespace iv::graph_input_lanes_details {
constexpr std::string_view metadata_dsp_graph = "dsp_graph";
constexpr std::string_view metadata_graph_input = "dsp_graph.graph_input";
constexpr std::string_view metadata_input = "dsp_graph.input";
constexpr std::string_view metadata_knob = "dsp_graph.knob";
constexpr std::string_view metadata_virtual = "dsp_graph.virtual";
// UI-facing terminology deliberately calls a NodeBundle a concrete node.
// The internal model remains NodeBundle; this metadata is its presentation
// contract for the side panel and timeline-lane consumers.
constexpr std::string_view metadata_node_bundle = "dsp_graph.concrete";
constexpr std::string_view metadata_sample = "dsp_graph.sample";
constexpr std::string_view metadata_event = "dsp_graph.event";
constexpr std::string_view metadata_module_instance_id = "dsp_graph.module_instance_id";
constexpr std::string_view metadata_virtual_node_id = "dsp_graph.virtual_node_id";
constexpr std::string_view metadata_node_bundle_node_id = "dsp_graph.concrete_node_id";
constexpr std::string_view metadata_port_kind = "dsp_graph.port_kind";
constexpr std::string_view metadata_port_ordinal = "dsp_graph.port_ordinal";
constexpr std::string_view metadata_channel_type = "dsp_graph.channel_type";
constexpr std::string_view metadata_node_bundle_port_ordinal = "dsp_graph.member_ordinal";
constexpr std::string_view metadata_graph_output = "dsp_graph.graph_output";
constexpr std::string_view metadata_output = "dsp_graph.output";
constexpr std::string_view metadata_public = "dsp_graph.public";
constexpr std::string_view metadata_public_input = "dsp_graph.public_input";
constexpr std::string_view metadata_public_output = "dsp_graph.public_output";
constexpr std::string_view metadata_public_source_id = "dsp_graph.public_source_id";
constexpr std::string_view metadata_public_port_name = "dsp_graph.public_port_name";

inline std::string public_source_identity(GraphInputLanes::DesiredPublicGraphPort const& port)
{
    if (!port.source_identity.empty()) {
        return port.source_identity;
    }
    if (port.source_infos.empty()) {
        return {};
    }
    return port.source_infos.front().declaration_identity;
}

inline std::optional<int> public_source_identity_hash(GraphInputLanes::DesiredPublicGraphPort const& port)
{
    if (port.source_identity_hash.has_value()) {
        return port.source_identity_hash;
    }
    auto const source_identity = public_source_identity(port);
    if (source_identity.empty()) {
        return std::nullopt;
    }
    return static_cast<int>(std::hash<std::string>{}(source_identity));
}

inline std::optional<int> public_port_name_hash(GraphInputLanes::DesiredPublicGraphPort const& port)
{
    if (port.public_port_name_hash.has_value()) {
        return port.public_port_name_hash;
    }
    if (port.port_name.empty()) {
        return std::nullopt;
    }
    return static_cast<int>(std::hash<std::string>{}(port.port_name));
}

inline std::string runtime_virtual_node_id(
    std::string_view instance_id,
    std::string_view virtual_node_id);

inline int local_hash_string(std::string const &value)
{
    return static_cast<int>(std::hash<std::string>{}(value));
}

inline LaneId stable_lane_id_for_key(std::string const &key)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : key) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }
    hash &= 0x7fffffffffffffffull;
    if (hash == 0) {
        hash = 1;
    }
    return LaneId{hash};
}

inline NodeRef add_graph_sample_output_sink(
    GraphBuilder& builder,
    LaneId lane,
    ChannelTypeId channel_type)
{
    switch (channel_type) {
    case ChannelTypeId::mono:
        return builder.node<GraphSampleOutputSink<ChannelTypeId::mono>>(lane);
    case ChannelTypeId::stereo:
        return builder.node<GraphSampleOutputSink<ChannelTypeId::stereo>>(lane);
    case ChannelTypeId::count:
        break;
    }
    details::error("invalid channel type for graph sample output sink");
}

inline void emit_debug_message(std::string message)
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectMessageNotification{
            .level = "debug",
            .message = std::move(message),
        }));
}

inline void append_graph_input_port_descriptors(
    std::vector<GraphInputLanes::DesiredGraphInputPort> &ports,
    std::string const &instance_id,
    int module_instance_id,
    std::string const &virtual_node_id,
    std::optional<size_t> node_bundle_port_ordinal,
    PortKind port_kind,
    std::span<IntrospectionPortInfo const> virtual_ports)
{
    ports.reserve(ports.size() + virtual_ports.size());
    for (auto const &port : virtual_ports) {
        ports.push_back(GraphInputLanes::DesiredGraphInputPort{
            .instance_id = instance_id,
            .module_instance_id = module_instance_id,
            .port = GraphInputPortDescriptor{
                .virtual_node_id = virtual_node_id,
                .node_bundle_port_ordinal = node_bundle_port_ordinal,
                .port_kind = port_kind,
                .port_ordinal = port.ordinal,
                .port_name = port.name,
                .port_type = port.type,
                .sample_channel_type = port.sample_channel_type,
            },
            .default_connected = port.connectivity != VirtualPortConnectivity::disconnected,
        });
    }
}

inline bool batch_has_changes(TimelineLaneBatchUpdate const &batch)
{
    return !batch.upserts.empty()
        || !batch.removals.empty()
        || !batch.connections_to_remove.empty()
        || !batch.connections_to_add.empty()
        || !batch.hierarchy_removals.empty()
        || !batch.hierarchy_additions.empty();
}

inline std::optional<ChannelTypeId> resolve_sample_channel_type(
    std::span<GraphInputLanes::DesiredGraphInputPort const> ports,
    std::string_view virtual_node_id,
    std::optional<size_t> node_bundle_port_ordinal,
    size_t port_ordinal)
{
    for (auto const &port : ports) {
        if (port.port.port_kind != PortKind::sample) {
            continue;
        }
        if (port.port.virtual_node_id != virtual_node_id) {
            continue;
        }
        if (port.port.node_bundle_port_ordinal != node_bundle_port_ordinal) {
            continue;
        }
        if (port.port.port_ordinal != port_ordinal) {
            continue;
        }
        return port.port.sample_channel_type;
    }
    return std::nullopt;
}

inline std::string virtual_knob_key(GraphInputLanes::DesiredGraphInputPort const &port)
{
    auto const virtual_node_id = local_hash_string(port.port.virtual_node_id);
    auto const node_bundle_port_id = local_hash_string(
        port.port.virtual_node_id
        + "\x1f"
        + "node-bundle:"
        + (port.port.node_bundle_port_ordinal.has_value()
            ? std::to_string(*port.port.node_bundle_port_ordinal)
            : std::string("virtual")));

    return "virtual-knob:instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "virtual:" + std::to_string(virtual_node_id)
        + "\x1f" + "node-bundle:" + std::to_string(node_bundle_port_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:"
        + (port.port.sample_channel_type.has_value()
            ? std::to_string(static_cast<int>(*port.port.sample_channel_type))
            : std::string("none"));
}

inline std::string virtual_event_input_key(GraphInputLanes::DesiredGraphInputPort const &port)
{
    auto const virtual_node_id = local_hash_string(port.port.virtual_node_id);
    auto const node_bundle_port_id = local_hash_string(
        port.port.virtual_node_id
        + "\x1f"
        + "member:virtual");

    return "virtual-event-input:instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "virtual:" + std::to_string(virtual_node_id)
        + "\x1f" + "node-bundle:" + std::to_string(node_bundle_port_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:none";
}

inline std::string sample_input_key(GraphInputLanes::DesiredGraphInputPort const &port)
{
    auto const virtual_node_id = local_hash_string(port.port.virtual_node_id);
    auto const node_bundle_port_id = local_hash_string(
        port.port.virtual_node_id
        + "\x1f"
        + "node-bundle:"
        + (port.port.node_bundle_port_ordinal.has_value()
            ? std::to_string(*port.port.node_bundle_port_ordinal)
            : std::string("virtual")));

    return "sample-input:instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "virtual:" + std::to_string(virtual_node_id)
        + "\x1f" + "node-bundle:" + std::to_string(node_bundle_port_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:"
        + (port.port.sample_channel_type.has_value()
            ? std::to_string(static_cast<int>(*port.port.sample_channel_type))
            : std::string("none"));
}

inline std::string event_input_key(GraphInputLanes::DesiredGraphInputPort const &port)
{
    auto const virtual_node_id = local_hash_string(port.port.virtual_node_id);
    auto const node_bundle_port_id = local_hash_string(
        port.port.virtual_node_id
        + "\x1f"
        + "node-bundle:"
        + (port.port.node_bundle_port_ordinal.has_value()
            ? std::to_string(*port.port.node_bundle_port_ordinal)
            : std::string("virtual")));

    return "event-input:instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "virtual:" + std::to_string(virtual_node_id)
        + "\x1f" + "node-bundle:" + std::to_string(node_bundle_port_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:none";
}

inline std::string output_identity_key(
    GraphInputLanes::DesiredGraphInputPort const &port,
    std::string_view role)
{
    auto const virtual_node_id = local_hash_string(port.port.virtual_node_id);
    auto const node_bundle_port_id = local_hash_string(
        port.port.virtual_node_id
        + "\x1f"
        + "node-bundle:"
        + (port.port.node_bundle_port_ordinal.has_value()
            ? std::to_string(*port.port.node_bundle_port_ordinal)
            : std::string("virtual")));

    return std::string(role) + ":instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "virtual:" + std::to_string(virtual_node_id)
        + "\x1f" + "node-bundle:" + std::to_string(node_bundle_port_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:"
        + (port.port.sample_channel_type.has_value()
            ? std::to_string(static_cast<int>(*port.port.sample_channel_type))
            : std::string("none"));
}

inline std::string public_port_identity_key(GraphInputLanes::DesiredPublicGraphPort const &port)
{
    // A public output lane represents the aggregate graph output. Source
    // spans identify controls for contributions to that lane, never lanes.
    auto const source_identity_hash = port.input ? public_source_identity_hash(port) : std::nullopt;
    return std::string(port.input ? "public-input" : "public-output")
        + ":instance:" + std::to_string(port.module_instance_id)
        + "\x1fkind:" + std::to_string(static_cast<int>(port.port_kind == PortKind::event))
        + (!source_identity_hash.has_value()
            ? "\x1fordinal:" + std::to_string(port.port_ordinal)
            : "\x1fsource:" + std::to_string(*source_identity_hash))
        + (!port.input && public_port_name_hash(port).has_value()
            ? "\x1fname:" + std::to_string(*public_port_name_hash(port))
            : "")
        + (port.input && (source_identity_hash.has_value() || port.node_bundle_port_ordinal.has_value())
            ? (port.node_bundle_port_ordinal.has_value()
                ? "\x1fmember:" + std::to_string(*port.node_bundle_port_ordinal)
                : "\x1fmember:virtual")
            : "");
}

inline std::string existing_identity_key(
    LaneMetadata const &metadata,
    std::string_view role)
{
    auto const module_instance_id = metadata.int_value(metadata_module_instance_id);
    auto const virtual_node_id = metadata.int_value(metadata_virtual_node_id);
    auto const node_bundle_port_id = metadata.int_value(metadata_node_bundle_node_id);
    auto const port_kind = metadata.int_value(metadata_port_kind);
    auto const port_ordinal = metadata.int_value(metadata_port_ordinal);
    auto const channel_type = metadata.int_value(metadata_channel_type);
    if (!module_instance_id.has_value()
        || !virtual_node_id.has_value()
        || !node_bundle_port_id.has_value()
        || !port_kind.has_value()
        || !port_ordinal.has_value()) {
        return {};
    }

    return std::string(role)
        + ":instance:" + std::to_string(*module_instance_id)
        + "\x1f" + "virtual:" + std::to_string(*virtual_node_id)
        + "\x1f" + "node-bundle:" + std::to_string(*node_bundle_port_id)
        + "\x1f" + "kind:" + std::to_string(*port_kind)
        + "\x1f" + "ordinal:" + std::to_string(*port_ordinal)
        + "\x1f" + "channel:"
        + (channel_type.has_value() ? std::to_string(*channel_type) : std::string("none"));
}

inline std::string runtime_virtual_node_id(
    std::string_view instance_id,
    std::string_view virtual_node_id)
{
    std::string value(instance_id);
    value += "\x1fvirtual:";
    value += virtual_node_id;
    return value;
}

inline InternedString lane_external_id_or_new(
    std::unordered_map<std::string, InternedString> const &stored_ids_by_key,
    std::unordered_map<std::string, GraphInputLanes::ExistingTrackedLane> const &existing_by_key,
    std::string const &key)
{
    if (auto const it = stored_ids_by_key.find(key); it != stored_ids_by_key.end()) {
        return it->second;
    }
    if (auto const it = existing_by_key.find(key); it != existing_by_key.end()
        && !it->second.external_id.empty()) {
        return it->second.external_id;
    }
    return generate_uuid_v4();
}

inline TypeErasedLaneNode make_sample_input_node(Sample default_value, std::string name = "value")
{
    return TypeErasedLaneNode(GraphSampleInputLaneNode{
        .default_value = default_value,
        .name = std::move(name),
    });
}

inline GraphInputPortDescriptor sample_output_descriptor(
    GraphBuilder::VirtualSampleOutputFamily const &output,
    std::optional<size_t> member_ordinal)
{
    return GraphInputPortDescriptor{
        .virtual_node_id = output.virtual_node_id,
        .node_bundle_port_ordinal = member_ordinal,
        .port_kind = PortKind::sample,
        .port_ordinal = output.family_ordinal,
        .port_name = output.family_name,
        .port_type = "sample",
        .sample_channel_type = output.channel_type,
    };
}

inline GraphInputPortDescriptor event_output_descriptor(
    GraphBuilder::VirtualEventOutput const &output,
    std::optional<size_t> member_ordinal)
{
    return GraphInputPortDescriptor{
        .virtual_node_id = output.virtual_node_id,
        .node_bundle_port_ordinal = member_ordinal,
        .port_kind = PortKind::event,
        .port_ordinal = output.source.port_ordinal,
        .port_name = output.config.name,
        .port_type = details::event_type_name(output.config.type),
    };
}

inline GraphInputPortDescriptor sample_port_descriptor(
    GraphBuilder::VirtualSampleInputFamily const &input,
    std::optional<size_t> member_ordinal)
{
    return GraphInputPortDescriptor{
        .virtual_node_id = input.virtual_node_id,
        .node_bundle_port_ordinal = member_ordinal,
        .port_kind = PortKind::sample,
        .port_ordinal = input.family_ordinal,
        .port_name = input.family_name,
        .port_type = "sample",
        .sample_channel_type = input.channel_type,
    };
}

inline GraphInputPortDescriptor event_port_descriptor(
    GraphBuilder::VirtualEventInput const &input,
    std::optional<size_t> member_ordinal)
{
    return GraphInputPortDescriptor{
        .virtual_node_id = input.virtual_node_id,
        .node_bundle_port_ordinal = member_ordinal,
        .port_kind = PortKind::event,
        .port_ordinal = input.target.port_ordinal,
        .port_name = input.config.name,
        .port_type = details::event_type_name(input.config.type),
    };
}

inline GraphInputPortDescriptor with_runtime_virtual_node_id(
    GraphInputPortDescriptor descriptor,
    std::string_view instance_id)
{
    descriptor.virtual_node_id = runtime_virtual_node_id(
        instance_id,
        descriptor.virtual_node_id);
    return descriptor;
}

inline bool builder_has_graph_port_descriptions(GraphBuilder const &builder)
{
    auto const ports = builder.virtual_ports();
    return !ports.sample_inputs.empty()
        || !ports.event_inputs.empty()
        || !ports.sample_outputs.empty()
        || !ports.event_outputs.empty()
        || !builder.public_sample_input_families().families.empty()
        || !builder.public_event_inputs().empty()
        || !builder.public_sample_output_families().families.empty()
        || !builder.public_event_outputs().empty();
}

} // namespace iv::graph_input_lanes_details
