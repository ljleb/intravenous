#include <intravenous/runtime/graph_input_lanes.h>

#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_lane_nodes/controls.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/task_ids.h>
#include <intravenous/runtime/uuid.h>

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
constexpr std::string_view metadata_channel_type = "dsp_graph.channel_type";
constexpr std::string_view metadata_member_ordinal = "dsp_graph.member_ordinal";
constexpr std::string_view metadata_graph_output = "dsp_graph.graph_output";
constexpr std::string_view metadata_output = "dsp_graph.output";
constexpr std::string_view metadata_public = "dsp_graph.public";
constexpr std::string_view metadata_public_input = "dsp_graph.public_input";
constexpr std::string_view metadata_public_output = "dsp_graph.public_output";
constexpr std::string_view metadata_public_source_id = "dsp_graph.public_source_id";

std::string public_source_identity(GraphInputLanes::DesiredPublicGraphPort const& port)
{
    if (!port.source_identity.empty()) {
        return port.source_identity;
    }
    if (port.source_infos.empty()) {
        return {};
    }
    return port.source_infos.front().declaration_identity;
}

std::optional<int> public_source_identity_hash(GraphInputLanes::DesiredPublicGraphPort const& port)
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

std::string runtime_logical_node_id(
    std::string_view instance_id,
    std::string_view logical_node_id);

int local_hash_string(std::string const &value)
{
    return static_cast<int>(std::hash<std::string>{}(value));
}

LaneId stable_lane_id_for_key(std::string const &key)
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

void emit_debug_message(std::string message)
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectMessageNotification{
            .level = "debug",
            .message = std::move(message),
        }));
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
                .sample_channel_type = port.sample_channel_type,
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

std::optional<ChannelTypeId> resolve_sample_channel_type(
    std::span<GraphInputLanes::DesiredGraphInputPort const> ports,
    std::string_view logical_node_id,
    std::optional<size_t> concrete_member_ordinal,
    size_t port_ordinal)
{
    for (auto const &port : ports) {
        if (port.port.port_kind != PortKind::sample) {
            continue;
        }
        if (port.port.logical_node_id != logical_node_id) {
            continue;
        }
        if (port.port.concrete_member_ordinal != concrete_member_ordinal) {
            continue;
        }
        if (port.port.port_ordinal != port_ordinal) {
            continue;
        }
        return port.port.sample_channel_type;
    }
    return std::nullopt;
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
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:"
        + (port.port.sample_channel_type.has_value()
            ? std::to_string(static_cast<int>(*port.port.sample_channel_type))
            : std::string("none"));
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
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:none";
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
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:"
        + (port.port.sample_channel_type.has_value()
            ? std::to_string(static_cast<int>(*port.port.sample_channel_type))
            : std::string("none"));
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
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:none";
}

std::string output_identity_key(
    GraphInputLanes::DesiredGraphInputPort const &port,
    std::string_view role)
{
    auto const logical_node_id = local_hash_string(port.port.logical_node_id);
    auto const concrete_node_id = local_hash_string(
        port.port.logical_node_id
        + "\x1f"
        + "member:"
        + (port.port.concrete_member_ordinal.has_value()
            ? std::to_string(*port.port.concrete_member_ordinal)
            : std::string("logical")));

    return std::string(role) + ":instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "logical:" + std::to_string(logical_node_id)
        + "\x1f" + "concrete:" + std::to_string(concrete_node_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:"
        + (port.port.sample_channel_type.has_value()
            ? std::to_string(static_cast<int>(*port.port.sample_channel_type))
            : std::string("none"));
}

std::string public_port_identity_key(GraphInputLanes::DesiredPublicGraphPort const &port)
{
    auto const source_identity_hash = public_source_identity_hash(port);
    return std::string(port.input ? "public-input" : "public-output")
        + ":instance:" + std::to_string(port.module_instance_id)
        + "\x1fkind:" + std::to_string(static_cast<int>(port.port_kind == PortKind::event))
        + (!source_identity_hash.has_value()
            ? "\x1fordinal:" + std::to_string(port.port_ordinal)
            : "\x1fsource:" + std::to_string(*source_identity_hash))
        + (source_identity_hash.has_value() || port.concrete_member_ordinal.has_value()
            ? (port.concrete_member_ordinal.has_value()
                ? "\x1fmember:" + std::to_string(*port.concrete_member_ordinal)
                : "\x1fmember:logical")
            : "");
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
    auto const channel_type = metadata.int_value(metadata_channel_type);
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
        + "\x1f" + "ordinal:" + std::to_string(*port_ordinal)
        + "\x1f" + "channel:"
        + (channel_type.has_value() ? std::to_string(*channel_type) : std::string("none"));
}

std::string runtime_logical_node_id(
    std::string_view instance_id,
    std::string_view logical_node_id)
{
    std::string value(instance_id);
    value += "\x1flogical:";
    value += logical_node_id;
    return value;
}

InternedString lane_external_id_or_new(
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

TypeErasedLaneNode make_sample_input_node(Sample default_value)
{
    return TypeErasedLaneNode(GraphSampleInputLaneNode{
        .default_value = default_value,
    });
}

GraphInputPortDescriptor sample_output_descriptor(
    GraphBuilder::LogicalSampleOutputFamily const &output,
    std::optional<size_t> member_ordinal)
{
    return GraphInputPortDescriptor{
        .logical_node_id = output.logical_node_id,
        .concrete_member_ordinal = member_ordinal,
        .port_kind = PortKind::sample,
        .port_ordinal = output.family_ordinal,
        .port_name = output.family_name,
        .port_type = "sample",
        .sample_channel_type = output.channel_type,
    };
}

GraphInputPortDescriptor event_output_descriptor(
    GraphBuilder::LogicalEventOutput const &output,
    std::optional<size_t> member_ordinal)
{
    return GraphInputPortDescriptor{
        .logical_node_id = output.logical_node_id,
        .concrete_member_ordinal = member_ordinal,
        .port_kind = PortKind::event,
        .port_ordinal = output.source.port,
        .port_name = output.config.name,
        .port_type = details::event_type_name(output.config.type),
    };
}

GraphInputPortDescriptor sample_port_descriptor(
    GraphBuilder::LogicalSampleInputFamily const &input,
    std::optional<size_t> member_ordinal)
{
    return GraphInputPortDescriptor{
        .logical_node_id = input.logical_node_id,
        .concrete_member_ordinal = member_ordinal,
        .port_kind = PortKind::sample,
        .port_ordinal = input.family_ordinal,
        .port_name = input.family_name,
        .port_type = "sample",
        .sample_channel_type = input.channel_type,
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

GraphInputPortDescriptor with_runtime_logical_node_id(
    GraphInputPortDescriptor descriptor,
    std::string_view instance_id)
{
    descriptor.logical_node_id = runtime_logical_node_id(
        instance_id,
        descriptor.logical_node_id);
    return descriptor;
}

} // namespace

std::vector<GraphInputLanes::DesiredGraphInputPort>
GraphInputLanes::graph_input_port_descriptors_for(
    IvModuleInstance const &instance)
{
    std::vector<DesiredGraphInputPort> ports;
    auto const module_instance_id = module_instance_numeric_id(instance.instance_id);
    for (auto const &node : instance.introspection.logical_nodes) {
        auto const runtime_node_id = runtime_logical_node_id(instance.instance_id, node.id);
        append_graph_input_port_descriptors(
            ports,
            instance.instance_id,
            module_instance_id,
            runtime_node_id,
            std::nullopt,
            PortKind::sample,
            node.sample_inputs);
        append_graph_input_port_descriptors(
            ports,
            instance.instance_id,
            module_instance_id,
            runtime_node_id,
            std::nullopt,
            PortKind::event,
            node.event_inputs);
        for (auto const &member : node.members) {
            append_graph_input_port_descriptors(
                ports,
                instance.instance_id,
                module_instance_id,
                runtime_node_id,
                member.ordinal,
                PortKind::sample,
                member.sample_inputs);
            append_graph_input_port_descriptors(
                ports,
                instance.instance_id,
                module_instance_id,
                runtime_node_id,
                member.ordinal,
                PortKind::event,
                member.event_inputs);
        }
    }
    return ports;
}

std::vector<GraphInputLanes::DesiredGraphInputPort>
GraphInputLanes::graph_output_port_descriptors_for(
    IvModuleInstance const &instance)
{
    std::vector<DesiredGraphInputPort> ports;
    auto const module_instance_id = module_instance_numeric_id(instance.instance_id);
    for (auto const &node : instance.introspection.logical_nodes) {
        auto const runtime_node_id = runtime_logical_node_id(instance.instance_id, node.id);
        append_graph_input_port_descriptors(
            ports,
            instance.instance_id,
            module_instance_id,
            runtime_node_id,
            std::nullopt,
            PortKind::sample,
            node.sample_outputs);
        append_graph_input_port_descriptors(
            ports,
            instance.instance_id,
            module_instance_id,
            runtime_node_id,
            std::nullopt,
            PortKind::event,
            node.event_outputs);
        for (auto const &member : node.members) {
            append_graph_input_port_descriptors(
                ports,
                instance.instance_id,
                module_instance_id,
                runtime_node_id,
                member.ordinal,
                PortKind::sample,
                member.sample_outputs);
            append_graph_input_port_descriptors(
                ports,
                instance.instance_id,
                module_instance_id,
                runtime_node_id,
                member.ordinal,
                PortKind::event,
                member.event_outputs);
        }
    }
    return ports;
}

std::vector<GraphInputLanes::DesiredPublicGraphPort>
GraphInputLanes::public_graph_input_ports_for(
    std::string const &instance_id,
    GraphBuilder const &builder)
{
    std::vector<DesiredPublicGraphPort> ports;
    auto const module_instance_id = module_instance_numeric_id(instance_id);

    for (auto const &family : builder.public_sample_input_families().families) {
        std::vector<DesiredPublicGraphPortChannel> channels;
        channels.reserve(family.channels.size());
        for (auto const &channel : family.channels) {
            channels.push_back(DesiredPublicGraphPortChannel{
                .port_ordinal = channel.port_ordinal,
            });
        }
        ports.push_back(DesiredPublicGraphPort{
            .instance_id = instance_id,
            .module_instance_id = module_instance_id,
            .input = true,
            .port_kind = PortKind::sample,
            .port_ordinal = family.family_ordinal,
            .port_name = family.family_name,
            .port_type = "sample",
            .sample_channel_type = family.channel_type,
            .default_value = family.input_config.default_value,
            .min = family.input_config.min,
            .max = family.input_config.max,
            .source_infos = family.source_infos,
            .source_identity = family.source_infos.empty()
                ? std::string{}
                : family.source_infos.front().declaration_identity,
            .channels = std::move(channels),
        });
    }
    for (auto const &input : builder.public_event_inputs()) {
        ports.push_back(DesiredPublicGraphPort{
            .instance_id = instance_id,
            .module_instance_id = module_instance_numeric_id(instance_id),
            .input = true,
            .port_kind = PortKind::event,
            .port_ordinal = input.port_ordinal,
            .port_name = input.config.name,
            .port_type = details::event_type_name(input.config.type),
            .event_type = input.config.type,
        });
    }

    return ports;
}

std::vector<GraphInputLanes::DesiredPublicGraphPort>
GraphInputLanes::public_graph_output_ports_for(
    std::string const &instance_id,
    GraphBuilder const &builder)
{
    std::vector<DesiredPublicGraphPort> ports;
    auto const module_instance_id = module_instance_numeric_id(instance_id);

    for (auto const &family : builder.public_sample_output_families().families) {
        std::vector<DesiredPublicGraphPortChannel> channels;
        channels.reserve(family.channels.size());
        for (auto const &channel : family.channels) {
            channels.push_back(DesiredPublicGraphPortChannel{
                .port_ordinal = channel.port_ordinal,
            });
        }
        ports.push_back(DesiredPublicGraphPort{
            .instance_id = instance_id,
            .module_instance_id = module_instance_id,
            .input = false,
            .port_kind = PortKind::sample,
            .port_ordinal = family.family_ordinal,
            .port_name = family.family_name,
            .port_type = "sample",
            .sample_channel_type = family.channel_type,
            .channels = std::move(channels),
        });
    }
    for (auto const &output : builder.public_event_outputs()) {
        ports.push_back(DesiredPublicGraphPort{
            .instance_id = instance_id,
            .module_instance_id = module_instance_id,
            .input = false,
            .port_kind = PortKind::event,
            .port_ordinal = output.port_ordinal,
            .port_name = output.config.name,
            .port_type = details::event_type_name(output.config.type),
            .event_type = output.config.type,
        });
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
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:"
        + (port.port.sample_channel_type.has_value()
            ? std::to_string(static_cast<int>(*port.port.sample_channel_type))
            : std::string("none"));
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
    key += "\x1f" "channel:";
    key += port.sample_channel_type.has_value()
        ? std::to_string(static_cast<int>(*port.sample_channel_type))
        : "none";
    return key;
}

std::string GraphInputLanes::public_port_key(DesiredPublicGraphPort const &port)
{
    return public_port_identity_key(port);
}

std::string GraphInputLanes::public_sample_input_state_key(
    std::string_view instance_id,
    std::string_view source_identity,
    std::optional<size_t> member_ordinal)
{
    auto key = std::string(instance_id) + "\x1fpublic-source:" + std::string(source_identity);
    key += member_ordinal.has_value()
        ? "\x1fmember:" + std::to_string(*member_ordinal)
        : "\x1fmember:logical";
    return key;
}

std::string GraphInputLanes::public_port_external_id(DesiredPublicGraphPort const &port)
{
    auto id = "iv.public:"
        + std::string(port.input ? "input" : "output")
        + ":instance:" + port.instance_id
        + ":kind:" + std::string(port.port_kind == PortKind::sample ? "sample" : "event")
        + ":";
    if (auto const source_identity_hash = public_source_identity_hash(port); source_identity_hash.has_value()) {
        id += "source:" + std::to_string(*source_identity_hash);
        id += port.concrete_member_ordinal.has_value()
            ? ":member:" + std::to_string(*port.concrete_member_ordinal)
            : ":member:logical";
    } else {
        id += "ordinal:" + std::to_string(port.port_ordinal);
    }
    return id + ":channel:"
        + (port.sample_channel_type.has_value()
            ? std::to_string(static_cast<int>(*port.sample_channel_type))
            : std::string("none"));
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
    size_t input_ordinal,
    ChannelTypeId channel_type)
{
    return GraphInputPortDescriptor{
        .logical_node_id = node_id,
        .concrete_member_ordinal = member_ordinal,
        .port_kind = PortKind::sample,
        .port_ordinal = input_ordinal,
        .port_name = "",
        .port_type = "sample",
        .sample_channel_type = channel_type,
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
    if (port.port.sample_channel_type.has_value()) {
        metadata.set_int(
            std::string(metadata_channel_type),
            static_cast<int>(*port.port.sample_channel_type));
    }
    if (port.port.concrete_member_ordinal.has_value()) {
        metadata.set_int(
            std::string(metadata_member_ordinal),
            static_cast<int>(*port.port.concrete_member_ordinal));
    }
    return metadata;
}

LaneMetadata GraphInputLanes::graph_output_metadata(
    DesiredGraphInputPort const &port,
    bool logical,
    bool concrete,
    bool sample,
    bool event)
{
    LaneMetadata metadata;
    metadata.set_unit(std::string(metadata_dsp_graph));
    metadata.set_unit(std::string(metadata_graph_output));
    metadata.set_unit(std::string(metadata_output));
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
    if (port.port.sample_channel_type.has_value()) {
        metadata.set_int(
            std::string(metadata_channel_type),
            static_cast<int>(*port.port.sample_channel_type));
    }
    if (port.port.concrete_member_ordinal.has_value()) {
        metadata.set_int(
            std::string(metadata_member_ordinal),
            static_cast<int>(*port.port.concrete_member_ordinal));
    }
    return metadata;
}

LaneMetadata GraphInputLanes::public_graph_port_metadata(
    DesiredPublicGraphPort const &port,
    bool sample,
    bool event)
{
    LaneMetadata metadata;
    metadata.set_unit(std::string(metadata_dsp_graph));
    metadata.set_unit(std::string(metadata_public));
    metadata.set_unit(std::string(port.input ? metadata_public_input : metadata_public_output));
    metadata.set_unit(std::string(port.input ? metadata_graph_input : metadata_graph_output));
    metadata.set_unit(std::string(port.input ? metadata_input : metadata_output));
    if (sample) {
        metadata.set_unit(std::string(metadata_sample));
    }
    if (event) {
        metadata.set_unit(std::string(metadata_event));
    }
    metadata.set_int(std::string(metadata_module_instance_id), port.module_instance_id);
    metadata.set_int(
        std::string(metadata_port_kind),
        static_cast<int>(port.port_kind == PortKind::event));
    metadata.set_int(
        std::string(metadata_port_ordinal),
        static_cast<int>(port.port_ordinal));
    if (auto const source_identity_hash = public_source_identity_hash(port); source_identity_hash.has_value()) {
        metadata.set_int(
            std::string(metadata_public_source_id),
            *source_identity_hash);
    }
    if (port.concrete_member_ordinal.has_value()) {
        metadata.set_int(
            std::string(metadata_member_ordinal),
            static_cast<int>(*port.concrete_member_ordinal));
    }
    if (port.sample_channel_type.has_value()) {
        metadata.set_int(
            std::string(metadata_channel_type),
            static_cast<int>(*port.sample_channel_type));
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
    auto const channel_type = metadata.int_value(metadata_channel_type);
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
        && *port_ordinal == static_cast<int>(port.port.port_ordinal)
        && channel_type
            == (port.port.sample_channel_type.has_value()
                ? std::optional<int>(static_cast<int>(*port.port.sample_channel_type))
                : std::nullopt);
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
            .concrete_member_ordinal = [&]() -> std::optional<size_t> {
                auto const member = tracked.metadata.int_value(metadata_member_ordinal);
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
                        return make_sample_input_node(port.default_value);
                    }
                    if (port.port_kind == PortKind::event) {
                        return TypeErasedLaneNode(GraphEventOutputLaneNode{ .lane = lane });
                    }
                    return TypeErasedLaneNode(GraphSampleOutputLaneNode{ .lane = lane });
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
            ++reused_count;
            auto const existing_channel_type = it->second.metadata.int_value(metadata_channel_type);
            auto const desired_channel_type = port.sample_channel_type.has_value()
                ? std::optional<int>(static_cast<int>(*port.sample_channel_type))
                : std::nullopt;
            if (batch != nullptr && existing_channel_type != desired_channel_type) {
                batch->upserts.push_back(make_upsert(lane, it->second.external_id));
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
                        port.concrete_member_ordinal);
                auto const external_id_it = public_sample_input_lane_ids_by_key.find(external_id_state_key);
                batch->upserts.push_back(make_upsert(
                    lane,
                    external_id_it != public_sample_input_lane_ids_by_key.end()
                        ? external_id_it->second
                        : InternedString::from_string(public_port_external_id(port))));
            }
        }
        used_lane_ids.insert(lane.value);
    };

    std::unordered_set<std::string> reconciled_public_port_keys;
    for (auto const &port : desired_public_input_ports) {
        // Inputs without a source annotation retain the legacy one-port/one-
        // lane behavior.  Annotated inputs add a logical lane keyed by source
        // identity and, on demand, a lane for an individual evaluation.
        if (port.source_identity.empty()) {
            if (reconciled_public_port_keys.insert(public_port_key(port)).second) {
                reconcile_port(port);
            }
            continue;
        }

        auto const logical_state_key = public_sample_input_state_key(
            port.instance_id, port.source_identity, std::nullopt);
        auto const logical_state_it = public_sample_input_states_by_key.find(logical_state_key);
        auto const logical_state = logical_state_it == public_sample_input_states_by_key.end()
            ? ProjectPublicSampleInputState::timeline_lane
            : logical_state_it->second;
        if (logical_state == ProjectPublicSampleInputState::timeline_lane
            && reconciled_public_port_keys.insert(public_port_key(port)).second) {
            reconcile_port(port);
        }

        auto const member_state_key = public_sample_input_state_key(
            port.instance_id, port.source_identity, port.port_ordinal);
        auto const member_state_it = public_sample_input_states_by_key.find(member_state_key);
        if (member_state_it == public_sample_input_states_by_key.end()
            || member_state_it->second != ProjectPublicSampleInputState::timeline_lane) {
            continue;
        }
        auto concrete_port = port;
        concrete_port.concrete_member_ordinal = port.port_ordinal;
        if (reconciled_public_port_keys.insert(public_port_key(concrete_port)).second) {
            reconcile_port(concrete_port);
        }
    }
    for (auto const &port : desired_public_output_ports) {
        if (reconciled_public_port_keys.insert(public_port_key(port)).second) {
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

std::atomic<Sample::storage>& GraphInputLanes::ensure_public_sample_input_value_locked(
    std::string const &instance_id,
    std::string const &source_identity,
    Sample default_value)
{
    auto const key = public_sample_input_state_key(instance_id, source_identity, std::nullopt);
    auto [it, inserted] = public_sample_input_values.try_emplace(key);
    if (inserted) {
        it->second = std::make_unique<std::atomic<Sample::storage>>(default_value.value);
    }
    return *it->second;
}

std::optional<LaneId> GraphInputLanes::effective_public_sample_input_lane_locked(
    DesiredPublicGraphPort const &port) const
{
    if (port.port_kind != PortKind::sample || port.source_identity.empty()) {
        return public_graph_port_lane_for(port);
    }

    auto const logical_state_key = public_sample_input_state_key(
        port.instance_id, port.source_identity, std::nullopt);
    auto const logical_state_it = public_sample_input_states_by_key.find(logical_state_key);
    auto const logical_state = logical_state_it == public_sample_input_states_by_key.end()
        ? ProjectPublicSampleInputState::timeline_lane
        : logical_state_it->second;

    auto const member_state_key = public_sample_input_state_key(
        port.instance_id, port.source_identity, port.port_ordinal);
    auto const member_state_it = public_sample_input_states_by_key.find(member_state_key);
    auto const member_state = member_state_it == public_sample_input_states_by_key.end()
        ? ProjectPublicSampleInputState::logical_follow
        : member_state_it->second;

    if (member_state == ProjectPublicSampleInputState::disconnected) {
        return std::nullopt;
    }
    if (member_state == ProjectPublicSampleInputState::timeline_lane) {
        auto concrete_port = port;
        concrete_port.concrete_member_ordinal = port.port_ordinal;
        return public_graph_port_lane_for(concrete_port);
    }
    if (logical_state == ProjectPublicSampleInputState::disconnected) {
        return std::nullopt;
    }
    if (logical_state == ProjectPublicSampleInputState::overridden) {
        return std::nullopt;
    }
    return public_graph_port_lane_for(port);
}

std::optional<GraphInputLanes::ConcreteOutputState>
GraphInputLanes::effective_concrete_output_state_locked(
    DesiredGraphInputPort const &port) const
{
    auto logical_port = port;
    logical_port.port.concrete_member_ordinal = std::nullopt;
    bool const logical_is_timeline_lane =
        logical_output_is_timeline_lane_locked(logical_port);

    std::optional<ConcreteOutputState> state =
        logical_is_timeline_lane
            ? std::optional<ConcreteOutputState>(ConcreteOutputState::logical)
            : std::nullopt;
    if (auto const it = concrete_output_states_by_key.find(graph_input_port_key(port.port));
        it != concrete_output_states_by_key.end()) {
        state = it->second;
    }
    return state;
}

bool GraphInputLanes::logical_output_is_timeline_lane_locked(
    DesiredGraphInputPort const &port) const
{
    auto const it = logical_output_states_by_key.find(graph_input_port_key(port.port));
    return it != logical_output_states_by_key.end()
        && it->second == LogicalOutputState::timeline_lane;
}

void GraphInputLanes::reconcile_output_ports_locked(TimelineLaneBatchUpdate *batch)
{
    std::unordered_map<std::string, ExistingTrackedLane> logical_outputs_by_key;
    std::unordered_map<std::string, ExistingTrackedLane> concrete_outputs_by_key;
    std::unordered_set<std::uint64_t> used_lane_ids;

    for (auto const &tracked : tracked_lanes) {
        lane_ids.observe(tracked.lane);
        if (!tracked.metadata.has_unit(metadata_graph_output)
            || tracked.metadata.has_unit(metadata_public)) {
            continue;
        }
        if (tracked.metadata.has_unit(metadata_logical)) {
            logical_outputs_by_key.emplace(
                existing_identity_key(tracked.metadata, "logical-output"),
                tracked);
        } else if (tracked.metadata.has_unit(metadata_concrete)) {
            concrete_outputs_by_key.emplace(
                existing_identity_key(tracked.metadata, "concrete-output"),
                tracked);
        }
    }

    for (auto const &port : desired_output_ports) {
        if (port.port.concrete_member_ordinal.has_value()) {
            continue;
        }
        if (!logical_output_is_timeline_lane_locked(port)) {
            continue;
        }

        bool const is_event = port.port.port_kind == PortKind::event;
        auto const key = output_identity_key(port, "logical-output");
        LaneId lane {};
        if (auto const it = logical_outputs_by_key.find(key); it != logical_outputs_by_key.end()) {
            lane = it->second.lane;
        } else {
            lane = stable_lane_id_for_key(key);
            if (batch != nullptr) {
                auto metadata = graph_output_metadata(port, true, false, !is_event, is_event);
                auto external_id = lane_external_id_or_new(
                    logical_output_lane_ids_by_key,
                    logical_outputs_by_key,
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
        if (!port.port.concrete_member_ordinal.has_value()) {
            continue;
        }
        auto const state = effective_concrete_output_state_locked(port);
        if (state != ConcreteOutputState::timeline_lane) {
            continue;
        }

        bool const is_event = port.port.port_kind == PortKind::event;
        auto const key = output_identity_key(port, "concrete-output");
        LaneId lane {};
        if (auto const it = concrete_outputs_by_key.find(key); it != concrete_outputs_by_key.end()) {
            lane = it->second.lane;
        } else {
            lane = stable_lane_id_for_key(key);
            if (batch != nullptr) {
                auto metadata = graph_output_metadata(port, false, true, !is_event, is_event);
                auto external_id = lane_external_id_or_new(
                    concrete_output_lane_ids_by_key,
                    concrete_outputs_by_key,
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
    bool logical_aggregation)
{
    std::scoped_lock lock(mutex);
    for (auto const &tracked : tracked_lanes) {
        if (!tracked.metadata.has_unit(metadata_graph_output)) {
            continue;
        }
        if (logical_aggregation && !tracked.metadata.has_unit(metadata_logical)) {
            continue;
        }
        if (!logical_aggregation && !tracked.metadata.has_unit(metadata_concrete)) {
            continue;
        }
        if (tracked.metadata.int_value(metadata_logical_node_id) != hash_string(port.logical_node_id)) {
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
        auto const expected_member = port.concrete_member_ordinal.has_value()
            ? std::optional<int>(static_cast<int>(*port.concrete_member_ordinal))
            : std::nullopt;
        if (tracked.metadata.int_value(metadata_member_ordinal) != expected_member) {
            continue;
        }
        return tracked.lane;
    }
    return LaneId{};
}

void GraphInputLanes::mark_instances_requiring_output_rebuild_locked(
    std::string_view logical_node_id,
    std::optional<size_t> member_ordinal,
    size_t output_ordinal,
    PortKind port_kind)
{
    for (auto const &port : desired_output_ports) {
        if (port.port.port_kind != port_kind) {
            continue;
        }
        if (port.port.logical_node_id != logical_node_id) {
            continue;
        }
        if (port.port.port_ordinal != output_ordinal) {
            continue;
        }
        if (member_ordinal.has_value() && port.port.concrete_member_ordinal != member_ordinal) {
            continue;
        }
        pending_rebuild_instance_ids.insert(port.instance_id);
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
        if (!tracked.metadata.has_unit(metadata_graph_input)
            || tracked.metadata.has_unit(metadata_public)) {
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
                    live_input_value_or_locked(port.port.logical_node_id, port.port.port_ordinal, Sample{0.0f});
                auto external_id = lane_external_id_or_new(
                    logical_sample_knob_lane_ids_by_key,
                    logical_sample_knobs_by_key,
                    key);
                batch->upserts.push_back(TimelineLaneUpsert{
                    .lane = lane,
                    .external_id = external_id,
                    .make_node = [current_value] {
                        return TypeErasedLaneNode(KnobLaneNode{ .value = current_value });
                    },
                    .sample_channel_type = port.port.sample_channel_type,
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
                .sample_channel_type = port.port.sample_channel_type,
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
                    concrete_sample_input_lane_ids_by_key,
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

        auto const lane = stable_lane_id_for_key(key);
        auto const external_id = lane_external_id_or_new(
            logical_event_input_lane_ids_by_key,
            logical_event_inputs_by_key,
            key);
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
                .external_id = external_id,
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
                .external_id = external_id,
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
                        concrete_event_input_lane_ids_by_key,
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
    std::scoped_lock lock(output_blocks_mutex_);
    sample_output_blocks_[lane] = copy_sample_block(block.view());
}

void GraphInputLanes::publish_event_output_block(
    LaneId lane,
    std::span<TimedEvent const> events)
{
    std::scoped_lock lock(output_blocks_mutex_);
    auto &stored = event_output_blocks_[lane];
    stored.assign(events.begin(), events.end());
}

OwnedSampleBlock GraphInputLanes::sample_output_block(LaneId lane) const
{
    std::scoped_lock lock(output_blocks_mutex_);
    auto const it = sample_output_blocks_.find(lane);
    if (it == sample_output_blocks_.end()) {
        return {};
    }
    return it->second;
}

std::vector<TimedEvent> GraphInputLanes::event_output_block(LaneId lane) const
{
    std::scoped_lock lock(output_blocks_mutex_);
    auto const it = event_output_blocks_.find(lane);
    if (it == event_output_blocks_.end()) {
        return {};
    }
    return it->second;
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
            desired_output_ports_by_instance_id[created.instance->instance_id] =
                graph_output_port_descriptors_for(*created.instance);
            if (created.builder != nullptr) {
                desired_public_input_ports_by_instance_id[created.instance->instance_id] =
                    public_graph_input_ports_for(created.instance->instance_id, *created.builder);
                desired_public_output_ports_by_instance_id[created.instance->instance_id] =
                    public_graph_output_ports_for(created.instance->instance_id, *created.builder);
            }
        }
        for (auto const &updated : diff.updated) {
            if (updated.instance == nullptr) {
                continue;
            }
            desired_ports_by_instance_id[updated.instance->instance_id] =
                graph_input_port_descriptors_for(*updated.instance);
            desired_output_ports_by_instance_id[updated.instance->instance_id] =
                graph_output_port_descriptors_for(*updated.instance);
            if (updated.builder != nullptr) {
                desired_public_input_ports_by_instance_id[updated.instance->instance_id] =
                    public_graph_input_ports_for(updated.instance->instance_id, *updated.builder);
                desired_public_output_ports_by_instance_id[updated.instance->instance_id] =
                    public_graph_output_ports_for(updated.instance->instance_id, *updated.builder);
            }
        }
        for (auto const &deleted_instance_id : diff.deleted_instance_ids) {
            desired_ports_by_instance_id.erase(deleted_instance_id);
            desired_output_ports_by_instance_id.erase(deleted_instance_id);
            desired_public_input_ports_by_instance_id.erase(deleted_instance_id);
            desired_public_output_ports_by_instance_id.erase(deleted_instance_id);
        }
        refresh_desired_ports_locked();
        refresh_desired_output_ports_locked();
        refresh_desired_public_input_ports_locked();
        refresh_desired_public_output_ports_locked();
        (void)reconcile_ports_locked(&batch);
        reconcile_output_ports_locked(&batch);
        reconcile_public_ports_locked(&batch);
        queue_timeline_batch_locked(batch);
        if (ack_builder != nullptr) {
            ack_builder->set_version_index(current_update_version_index_);
        }
    }

    if (!diff.created.empty() || !diff.updated.empty() || !diff.deleted_instance_ids.empty()) {
        emit_debug_message(
            "graph input lanes builders changed: created="
            + std::to_string(diff.created.size())
            + " updated=" + std::to_string(diff.updated.size())
            + " deleted=" + std::to_string(diff.deleted_instance_ids.size())
            + " ackVersion="
            + std::to_string(ack_builder != nullptr ? current_update_version_index_ : diff.version_index));
    }

    for (auto const &created : diff.created) {
        if (created.instance == nullptr || created.builder == nullptr) {
            continue;
        }
        auto completion = complete_builder(created.instance->instance_id, *created.builder);
        emit_debug_message(
            "graph input lanes completed builder: instance=" + created.instance->instance_id
            + " prerequisiteLanes=" + std::to_string(completion.prerequisite_lanes.size())
            + " sampleInputs=" + std::to_string(completion.sample_inputs.size())
            + " eventInputs=" + std::to_string(completion.event_inputs.size())
            + " batchUpserts=" + std::to_string(completion.timeline_batch.upserts.size())
            + " batchRemovals=" + std::to_string(completion.timeline_batch.removals.size())
            + " batchConnectionsAdd=" + std::to_string(completion.timeline_batch.connections_to_add.size())
            + " batchConnectionsRemove=" + std::to_string(completion.timeline_batch.connections_to_remove.size()));
        if (ack_builder != nullptr) {
            ack_builder->set_prerequisite_lanes(
                created.instance->instance_id,
                completion.prerequisite_lanes);
        }
    }
    for (auto const &updated : diff.updated) {
        if (updated.instance == nullptr || updated.builder == nullptr) {
            continue;
        }
        auto completion = complete_builder(updated.instance->instance_id, *updated.builder);
        emit_debug_message(
            "graph input lanes recompleted builder: instance=" + updated.instance->instance_id
            + " prerequisiteLanes=" + std::to_string(completion.prerequisite_lanes.size())
            + " sampleInputs=" + std::to_string(completion.sample_inputs.size())
            + " eventInputs=" + std::to_string(completion.event_inputs.size())
            + " batchUpserts=" + std::to_string(completion.timeline_batch.upserts.size())
            + " batchRemovals=" + std::to_string(completion.timeline_batch.removals.size())
            + " batchConnectionsAdd=" + std::to_string(completion.timeline_batch.connections_to_add.size())
            + " batchConnectionsRemove=" + std::to_string(completion.timeline_batch.connections_to_remove.size()));
        if (ack_builder != nullptr) {
            ack_builder->set_prerequisite_lanes(
                updated.instance->instance_id,
                completion.prerequisite_lanes);
        }
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
                    && tracked.metadata.int_value(metadata_port_ordinal) == static_cast<int>(port.port_ordinal)
                    && tracked.metadata.int_value(metadata_channel_type)
                        == (port.sample_channel_type.has_value()
                            ? std::optional<int>(static_cast<int>(*port.sample_channel_type))
                            : std::nullopt);
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
                    && tracked.metadata.int_value(metadata_channel_type)
                        == (port.sample_channel_type.has_value()
                            ? std::optional<int>(static_cast<int>(*port.sample_channel_type))
                            : std::nullopt)
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
                    && tracked.metadata.int_value(metadata_channel_type)
                        == (port.sample_channel_type.has_value()
                            ? std::optional<int>(static_cast<int>(*port.sample_channel_type))
                            : std::nullopt)
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
            auto const sample_channel_type = resolve_sample_channel_type(
                desired_ports,
                request.node_id,
                request.member_ordinal,
                request.input_ordinal).value_or(ChannelTypeId::mono);
            auto const key = concrete_key(request.node_id, *request.member_ordinal);
            auto const port_key = graph_input_port_key(sample_input_descriptor(
                request.node_id,
                request.member_ordinal,
                request.input_ordinal,
                sample_channel_type));
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
            auto const sample_key = sample_input_key(DesiredGraphInputPort{
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
            auto const override_key = concrete_override_key(
                request.node_id,
                *request.member_ordinal,
                request.input_ordinal);

            switch (request.state) {
            case ProjectSampleInputState::default_:
                concrete_live_input_overrides.erase(override_key);
                concrete_sample_input_states_by_key.erase(port_key);
                concrete_sample_input_lane_ids_by_key.erase(sample_key);
                break;
            case ProjectSampleInputState::overridden:
                concrete_live_input_overrides.insert(override_key);
                concrete_sample_input_states_by_key[port_key] =
                    ConcreteSampleInputState::overridden;
                concrete_sample_input_lane_ids_by_key.erase(sample_key);
                break;
            case ProjectSampleInputState::logical_follow:
                concrete_live_input_overrides.erase(override_key);
                concrete_sample_input_states_by_key[port_key] =
                    ConcreteSampleInputState::logical_follow;
                concrete_sample_input_lane_ids_by_key.erase(sample_key);
                break;
            case ProjectSampleInputState::timeline_lane:
                concrete_live_input_overrides.erase(override_key);
                concrete_sample_input_states_by_key[port_key] =
                    ConcreteSampleInputState::timeline_lane;
                if (request.lane_id.has_value()) {
                    concrete_sample_input_lane_ids_by_key[sample_key] = *request.lane_id;
                } else {
                    concrete_sample_input_lane_ids_by_key.erase(sample_key);
                }
                break;
            case ProjectSampleInputState::disconnected:
                concrete_live_input_overrides.erase(override_key);
                concrete_sample_input_states_by_key[port_key] =
                    ConcreteSampleInputState::disconnected;
                concrete_sample_input_lane_ids_by_key.erase(sample_key);
                break;
            }

            mark_instances_requiring_rebuild_locked(
                request.node_id,
                request.member_ordinal,
                request.input_ordinal);
        } else {
            auto const logical_key = logical_knob_key(DesiredGraphInputPort{
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
                logical_sample_knob_states_by_key[port_key] =
                    LogicalSampleKnobState::overridden;
                logical_sample_knob_lane_ids_by_key.erase(logical_key);
                break;
            case ProjectSampleInputState::timeline_lane:
                logical_sample_knob_states_by_key[port_key] =
                    LogicalSampleKnobState::timeline_lane;
                if (request.lane_id.has_value()) {
                    logical_sample_knob_lane_ids_by_key[logical_key] = *request.lane_id;
                } else {
                    logical_sample_knob_lane_ids_by_key.erase(logical_key);
                }
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
            && request.state == ProjectPublicSampleInputState::default_) {
            public_sample_input_states_by_key.erase(state_key);
            public_sample_input_lane_ids_by_key.erase(state_key);
            pending_rebuild_instance_ids.insert(request.instance_id);
            reconcile_public_ports_locked(&batch);
            queue_timeline_batch_locked(batch);
            return;
        } else if (!request.member_ordinal.has_value()) {
            if (request.state != ProjectPublicSampleInputState::overridden
                && request.state != ProjectPublicSampleInputState::timeline_lane
                && request.state != ProjectPublicSampleInputState::disconnected) {
                throw std::runtime_error(
                    "logical public sample input state only supports overridden, timeline_lane, or disconnected");
            }
        } else if (request.state == ProjectPublicSampleInputState::default_) {
            public_sample_input_states_by_key.erase(state_key);
            public_sample_input_lane_ids_by_key.erase(state_key);
            pending_rebuild_instance_ids.insert(request.instance_id);
            reconcile_public_ports_locked(&batch);
            queue_timeline_batch_locked(batch);
            return;
        }

        public_sample_input_states_by_key[state_key] = request.state;
        if (request.state == ProjectPublicSampleInputState::timeline_lane
            && request.lane_id.has_value()) {
            // Lanes are keyed by public-port identity.  The state key has the
            // same logical/member partition and is deliberately used here so
            // callers can retain a chosen timeline external id.
            public_sample_input_lane_ids_by_key[state_key] = *request.lane_id;
        } else {
            public_sample_input_lane_ids_by_key.erase(state_key);
        }
        pending_rebuild_instance_ids.insert(request.instance_id);
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
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        auto const key = public_sample_input_state_key(instance_id, source_identity, std::nullopt);
        ensure_public_sample_input_value_locked(instance_id, source_identity, Sample{0.0f})
            .store(value.value, std::memory_order_relaxed);
        public_sample_input_states_by_key[key] = ProjectPublicSampleInputState::overridden;
        public_sample_input_lane_ids_by_key.erase(key);
        pending_rebuild_instance_ids.insert(instance_id);
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
                ? ProjectPublicSampleInputState::timeline_lane
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
                .logical_state = state == ProjectPublicSampleInputState::overridden ? "overridden"
                    : state == ProjectPublicSampleInputState::timeline_lane ? "timelineLane"
                    : state == ProjectPublicSampleInputState::disconnected ? "disconnected"
                    : "default",
            });
        }
        result[it->second].member_ordinals.push_back(port.port_ordinal);
    }
    return result;
}

void GraphInputLanes::set_event_input_state(
    ProjectSetEventInputStateRequest const &request)
{
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        auto const concrete_key_value = event_input_key(DesiredGraphInputPort{
            .instance_id = {},
            .module_instance_id = 0,
            .port = GraphInputPortDescriptor{
                .logical_node_id = request.node_id,
                .concrete_member_ordinal = request.member_ordinal,
                .port_kind = PortKind::event,
                .port_ordinal = request.input_ordinal,
            },
        });
        auto const logical_key_value = logical_event_input_key(DesiredGraphInputPort{
            .instance_id = {},
            .module_instance_id = 0,
            .port = GraphInputPortDescriptor{
                .logical_node_id = request.node_id,
                .concrete_member_ordinal = std::nullopt,
                .port_kind = PortKind::event,
                .port_ordinal = request.input_ordinal,
            },
        });
        auto const port_key = graph_input_port_key(GraphInputPortDescriptor{
            .logical_node_id = request.node_id,
            .concrete_member_ordinal = request.member_ordinal,
            .port_kind = PortKind::event,
            .port_ordinal = request.input_ordinal,
        });

        switch (request.state) {
        case ProjectEventInputState::default_:
            concrete_event_input_states_by_key.erase(port_key);
            concrete_event_input_lane_ids_by_key.erase(concrete_key_value);
            break;
        case ProjectEventInputState::logical_follow:
            concrete_event_input_states_by_key[port_key] =
                ConcreteEventInputState::logical_follow;
            concrete_event_input_lane_ids_by_key.erase(concrete_key_value);
            if (request.lane_id.has_value()) {
                logical_event_input_lane_ids_by_key[logical_key_value] = *request.lane_id;
            }
            break;
        case ProjectEventInputState::timeline_lane:
            concrete_event_input_states_by_key[port_key] =
                ConcreteEventInputState::timeline_lane;
            if (request.lane_id.has_value()) {
                concrete_event_input_lane_ids_by_key[concrete_key_value] = *request.lane_id;
            } else {
                concrete_event_input_lane_ids_by_key.erase(concrete_key_value);
            }
            break;
        case ProjectEventInputState::disconnected:
            concrete_event_input_states_by_key[port_key] =
                ConcreteEventInputState::disconnected;
            concrete_event_input_lane_ids_by_key.erase(concrete_key_value);
            break;
        }

        bool has_logical_follow_remaining = false;
        for (auto const &port : desired_ports) {
            if (port.port.port_kind != PortKind::event) {
                continue;
            }
            if (port.port.logical_node_id != request.node_id) {
                continue;
            }
            if (port.port.port_ordinal != request.input_ordinal) {
                continue;
            }
            auto const candidate_port_key = graph_input_port_key(GraphInputPortDescriptor{
                .logical_node_id = port.port.logical_node_id,
                .concrete_member_ordinal = port.port.concrete_member_ordinal,
                .port_kind = PortKind::event,
                .port_ordinal = port.port.port_ordinal,
            });
            if (auto const it = concrete_event_input_states_by_key.find(candidate_port_key);
                it != concrete_event_input_states_by_key.end()
                && it->second == ConcreteEventInputState::logical_follow) {
                has_logical_follow_remaining = true;
                break;
            }
        }
        if (!has_logical_follow_remaining) {
            logical_event_input_lane_ids_by_key.erase(logical_key_value);
        }

        mark_instances_requiring_rebuild_locked(
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
        auto const identity_key = output_identity_key(
            DesiredGraphInputPort{
                .instance_id = {},
                .module_instance_id = 0,
                .port = GraphInputPortDescriptor{
                    .logical_node_id = request.node_id,
                    .concrete_member_ordinal = request.member_ordinal,
                    .port_kind = PortKind::sample,
                    .port_ordinal = request.output_ordinal,
                    .sample_channel_type = resolve_sample_channel_type(
                        desired_output_ports,
                        request.node_id,
                        request.member_ordinal,
                        request.output_ordinal).value_or(ChannelTypeId::mono),
                },
            },
            request.member_ordinal.has_value() ? "concrete-output" : "logical-output");
        auto const sample_channel_type = resolve_sample_channel_type(
            desired_output_ports,
            request.node_id,
            request.member_ordinal,
            request.output_ordinal).value_or(ChannelTypeId::mono);
        auto const port_key = graph_input_port_key(GraphInputPortDescriptor{
            .logical_node_id = request.node_id,
            .concrete_member_ordinal = request.member_ordinal,
            .port_kind = PortKind::sample,
            .port_ordinal = request.output_ordinal,
            .sample_channel_type = sample_channel_type,
        });

        if (request.member_ordinal.has_value()) {
            switch (request.state) {
            case ProjectSampleOutputState::disconnected:
                concrete_output_states_by_key.erase(port_key);
                concrete_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectSampleOutputState::logical:
                concrete_output_states_by_key[port_key] = ConcreteOutputState::logical;
                concrete_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectSampleOutputState::timeline_lane:
                concrete_output_states_by_key[port_key] = ConcreteOutputState::timeline_lane;
                if (request.lane_id.has_value()) {
                    concrete_output_lane_ids_by_key[identity_key] = *request.lane_id;
                } else {
                    concrete_output_lane_ids_by_key.erase(identity_key);
                }
                break;
            }
        } else {
            switch (request.state) {
            case ProjectSampleOutputState::disconnected:
                logical_output_states_by_key.erase(port_key);
                logical_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectSampleOutputState::timeline_lane:
                logical_output_states_by_key[port_key] = LogicalOutputState::timeline_lane;
                if (request.lane_id.has_value()) {
                    logical_output_lane_ids_by_key[identity_key] = *request.lane_id;
                } else {
                    logical_output_lane_ids_by_key.erase(identity_key);
                }
                break;
            case ProjectSampleOutputState::logical:
                throw std::runtime_error(
                    "logical sample output state only supports disconnected or timeline_lane");
            }
        }

        mark_instances_requiring_output_rebuild_locked(
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
        auto const identity_key = output_identity_key(
            DesiredGraphInputPort{
                .instance_id = {},
                .module_instance_id = 0,
                .port = GraphInputPortDescriptor{
                    .logical_node_id = request.node_id,
                    .concrete_member_ordinal = request.member_ordinal,
                    .port_kind = PortKind::event,
                    .port_ordinal = request.output_ordinal,
                },
            },
            request.member_ordinal.has_value() ? "concrete-output" : "logical-output");
        auto const port_key = graph_input_port_key(GraphInputPortDescriptor{
            .logical_node_id = request.node_id,
            .concrete_member_ordinal = request.member_ordinal,
            .port_kind = PortKind::event,
            .port_ordinal = request.output_ordinal,
        });

        if (request.member_ordinal.has_value()) {
            switch (request.state) {
            case ProjectEventOutputState::disconnected:
                concrete_output_states_by_key.erase(port_key);
                concrete_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectEventOutputState::logical:
                concrete_output_states_by_key[port_key] = ConcreteOutputState::logical;
                concrete_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectEventOutputState::timeline_lane:
                concrete_output_states_by_key[port_key] = ConcreteOutputState::timeline_lane;
                if (request.lane_id.has_value()) {
                    concrete_output_lane_ids_by_key[identity_key] = *request.lane_id;
                } else {
                    concrete_output_lane_ids_by_key.erase(identity_key);
                }
                break;
            }
        } else {
            switch (request.state) {
            case ProjectEventOutputState::disconnected:
                logical_output_states_by_key.erase(port_key);
                logical_output_lane_ids_by_key.erase(identity_key);
                break;
            case ProjectEventOutputState::timeline_lane:
                logical_output_states_by_key[port_key] = LogicalOutputState::timeline_lane;
                if (request.lane_id.has_value()) {
                    logical_output_lane_ids_by_key[identity_key] = *request.lane_id;
                } else {
                    logical_output_lane_ids_by_key.erase(identity_key);
                }
                break;
            case ProjectEventOutputState::logical:
                throw std::runtime_error(
                    "logical event output state only supports disconnected or timeline_lane");
            }
        }

        mark_instances_requiring_output_rebuild_locked(
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
    std::unordered_map<std::string, InternedString> logical_sample_knob_external_ids_by_key;
    std::unordered_map<std::string, InternedString> sample_input_external_ids_by_key;
    std::unordered_map<std::string, InternedString> logical_event_input_external_ids_by_key;
    std::unordered_map<std::string, InternedString> event_input_external_ids_by_key;
    std::unordered_map<std::string, InternedString> logical_output_external_ids_by_key;
    std::unordered_map<std::string, InternedString> concrete_output_external_ids_by_key;

    // Public inputs are persisted through the existing graph sample-input
    // commands, using their synthetic source-query node id.  Project loading
    // bypasses socket RPC, so the runtime-project bridge recognizes that id
    // and routes it back to the public-input model.
    std::unordered_set<std::string> persisted_public_logical_inputs;
    for (auto const &port : desired_public_input_ports) {
        if (port.port_kind != PortKind::sample || port.source_identity.empty()) {
            continue;
        }
        auto const logical_key = public_sample_input_state_key(
            port.instance_id, port.source_identity, std::nullopt);
        auto const synthetic_node_id = "iv.public-input:" + port.instance_id + ":" + port.source_identity;
        if (persisted_public_logical_inputs.insert(logical_key).second) {
            auto const state_it = public_sample_input_states_by_key.find(logical_key);
            auto const state = state_it == public_sample_input_states_by_key.end()
                ? ProjectPublicSampleInputState::timeline_lane
                : state_it->second;
            if (state == ProjectPublicSampleInputState::overridden) {
                auto const value_it = public_sample_input_values.find(logical_key);
                snapshot.sample_input_values.push_back(ProjectSetSampleInputValueRequest{
                    .node_id = synthetic_node_id,
                    .input_ordinal = 0,
                    .value = value_it == public_sample_input_values.end()
                        ? port.default_value
                        : Sample{value_it->second->load(std::memory_order_relaxed)},
                });
            } else if (state != ProjectPublicSampleInputState::timeline_lane) {
                snapshot.sample_input_states.push_back(ProjectSetSampleInputStateRequest{
                    .node_id = synthetic_node_id,
                    .input_ordinal = 0,
                    .state = state == ProjectPublicSampleInputState::disconnected
                        ? ProjectSampleInputState::disconnected
                        : ProjectSampleInputState::timeline_lane,
                });
            }
        }

        auto const member_key = public_sample_input_state_key(
            port.instance_id, port.source_identity, port.port_ordinal);
        if (auto const state_it = public_sample_input_states_by_key.find(member_key);
            state_it != public_sample_input_states_by_key.end()) {
            ProjectSampleInputState state = ProjectSampleInputState::logical_follow;
            switch (state_it->second) {
            case ProjectPublicSampleInputState::default_:
            case ProjectPublicSampleInputState::logical_follow:
                state = ProjectSampleInputState::logical_follow;
                break;
            case ProjectPublicSampleInputState::timeline_lane:
                state = ProjectSampleInputState::timeline_lane;
                break;
            case ProjectPublicSampleInputState::disconnected:
                state = ProjectSampleInputState::disconnected;
                break;
            case ProjectPublicSampleInputState::overridden:
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

    for (auto const &tracked : tracked_lanes) {
        if (tracked.external_id.empty()) {
            continue;
        }
        if (tracked.metadata.has_unit(metadata_graph_input)) {
            if (tracked.metadata.has_unit(metadata_knob)
                && tracked.metadata.has_unit(metadata_logical)
                && tracked.metadata.has_unit(metadata_sample)) {
                logical_sample_knob_external_ids_by_key.emplace(
                    existing_identity_key(tracked.metadata, "logical-knob"),
                    tracked.external_id);
            } else if (tracked.metadata.has_unit(metadata_input)
                && tracked.metadata.has_unit(metadata_logical)
                && tracked.metadata.has_unit(metadata_event)) {
                logical_event_input_external_ids_by_key.emplace(
                    existing_identity_key(tracked.metadata, "logical-event-input"),
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
            if (tracked.metadata.has_unit(metadata_logical)) {
                logical_output_external_ids_by_key.emplace(
                    existing_identity_key(tracked.metadata, "logical-output"),
                    tracked.external_id);
            } else if (tracked.metadata.has_unit(metadata_concrete)) {
                concrete_output_external_ids_by_key.emplace(
                    existing_identity_key(tracked.metadata, "concrete-output"),
                    tracked.external_id);
            }
        }
    }

    for (auto const &port : desired_ports) {
        if (port.port.port_kind == PortKind::sample) {
            if (!port.port.concrete_member_ordinal.has_value()) {
                auto const state_key = graph_input_port_key(port.port);
                if (auto const state_it = logical_sample_knob_states_by_key.find(state_key);
                    state_it != logical_sample_knob_states_by_key.end()) {
                    if (state_it->second == LogicalSampleKnobState::timeline_lane) {
                        snapshot.sample_input_states.push_back(ProjectSetSampleInputStateRequest{
                            .node_id = port.port.logical_node_id,
                            .member_ordinal = std::nullopt,
                            .input_ordinal = port.port.port_ordinal,
                            .state = ProjectSampleInputState::timeline_lane,
                            .lane_id = [&]() -> std::optional<InternedString> {
                                auto const key = logical_knob_key(port);
                                if (auto const it = logical_sample_knob_external_ids_by_key.find(key);
                                    it != logical_sample_knob_external_ids_by_key.end()) {
                                    return it->second;
                                }
                                return std::nullopt;
                            }(),
                        });
                    } else {
                        snapshot.sample_input_values.push_back(ProjectSetSampleInputValueRequest{
                            .node_id = port.port.logical_node_id,
                            .member_ordinal = std::nullopt,
                            .input_ordinal = port.port.port_ordinal,
                            .value = live_input_value_or_locked(
                                port.port.logical_node_id,
                                port.port.port_ordinal,
                                Sample{0.0f}),
                        });
                    }
                }
                continue;
            }

            auto const state_key = graph_input_port_key(port.port);
            if (auto const state_it = concrete_sample_input_states_by_key.find(state_key);
                state_it != concrete_sample_input_states_by_key.end()) {
                auto const member_ordinal = *port.port.concrete_member_ordinal;
                if (state_it->second == ConcreteSampleInputState::overridden) {
                    snapshot.sample_input_values.push_back(ProjectSetSampleInputValueRequest{
                        .node_id = port.port.logical_node_id,
                        .member_ordinal = member_ordinal,
                        .input_ordinal = port.port.port_ordinal,
                        .value = live_input_value_or_locked(
                            port.port.logical_node_id,
                            member_ordinal,
                            port.port.port_ordinal,
                            Sample{0.0f}),
                    });
                } else {
                    ProjectSampleInputState state = ProjectSampleInputState::default_;
                    switch (state_it->second) {
                    case ConcreteSampleInputState::overridden:
                        state = ProjectSampleInputState::overridden;
                        break;
                    case ConcreteSampleInputState::logical_follow:
                        state = ProjectSampleInputState::logical_follow;
                        break;
                    case ConcreteSampleInputState::timeline_lane:
                        state = ProjectSampleInputState::timeline_lane;
                        break;
                    case ConcreteSampleInputState::disconnected:
                        state = ProjectSampleInputState::disconnected;
                        break;
                    }
                    snapshot.sample_input_states.push_back(ProjectSetSampleInputStateRequest{
                        .node_id = port.port.logical_node_id,
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
        if (auto const state_it = concrete_event_input_states_by_key.find(event_state_key);
            state_it != concrete_event_input_states_by_key.end()) {
            ProjectEventInputState state = ProjectEventInputState::default_;
            switch (state_it->second) {
            case ConcreteEventInputState::default_:
                state = ProjectEventInputState::default_;
                break;
            case ConcreteEventInputState::logical_follow:
                state = ProjectEventInputState::logical_follow;
                break;
            case ConcreteEventInputState::timeline_lane:
                state = ProjectEventInputState::timeline_lane;
                break;
            case ConcreteEventInputState::disconnected:
                state = ProjectEventInputState::disconnected;
                break;
            }
            snapshot.event_input_states.push_back(ProjectSetEventInputStateRequest{
                .node_id = port.port.logical_node_id,
                .member_ordinal = port.port.concrete_member_ordinal,
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
                    if (state == ProjectEventInputState::logical_follow) {
                        auto logical_port = port;
                        logical_port.port.concrete_member_ordinal = std::nullopt;
                        auto const key = logical_event_input_key(logical_port);
                        if (auto const it = logical_event_input_external_ids_by_key.find(key);
                            it != logical_event_input_external_ids_by_key.end()) {
                            return it->second;
                        }
                    }
                    return std::nullopt;
                }(),
            });
        }
    }

    for (auto const &port : desired_output_ports) {
        auto const output_state_key = graph_input_port_key(port.port);
        if (port.port.port_kind == PortKind::sample) {
            if (port.port.concrete_member_ordinal.has_value()) {
                if (auto const it = concrete_output_states_by_key.find(output_state_key);
                    it != concrete_output_states_by_key.end()) {
                    snapshot.sample_output_states.push_back(ProjectSetSampleOutputStateRequest{
                        .node_id = port.port.logical_node_id,
                        .member_ordinal = port.port.concrete_member_ordinal,
                        .output_ordinal = port.port.port_ordinal,
                        .state = it->second == ConcreteOutputState::logical
                            ? ProjectSampleOutputState::logical
                            : ProjectSampleOutputState::timeline_lane,
                        .lane_id = it->second == ConcreteOutputState::timeline_lane
                            ? [&]() -> std::optional<InternedString> {
                                auto const key = output_identity_key(port, "concrete-output");
                                if (auto const lane_it = concrete_output_external_ids_by_key.find(key);
                                    lane_it != concrete_output_external_ids_by_key.end()) {
                                    return lane_it->second;
                                }
                                return std::nullopt;
                            }()
                            : std::nullopt,
                    });
                }
            } else if (auto const it = logical_output_states_by_key.find(output_state_key);
                it != logical_output_states_by_key.end()) {
                (void)it;
                snapshot.sample_output_states.push_back(ProjectSetSampleOutputStateRequest{
                    .node_id = port.port.logical_node_id,
                    .member_ordinal = std::nullopt,
                    .output_ordinal = port.port.port_ordinal,
                    .state = ProjectSampleOutputState::timeline_lane,
                    .lane_id = [&]() -> std::optional<InternedString> {
                        auto const key = output_identity_key(port, "logical-output");
                        if (auto const lane_it = logical_output_external_ids_by_key.find(key);
                            lane_it != logical_output_external_ids_by_key.end()) {
                            return lane_it->second;
                        }
                        return std::nullopt;
                    }(),
                });
            }
            continue;
        }

        if (port.port.concrete_member_ordinal.has_value()) {
            if (auto const it = concrete_output_states_by_key.find(output_state_key);
                it != concrete_output_states_by_key.end()) {
                snapshot.event_output_states.push_back(ProjectSetEventOutputStateRequest{
                    .node_id = port.port.logical_node_id,
                    .member_ordinal = port.port.concrete_member_ordinal,
                    .output_ordinal = port.port.port_ordinal,
                    .state = it->second == ConcreteOutputState::logical
                        ? ProjectEventOutputState::logical
                        : ProjectEventOutputState::timeline_lane,
                    .lane_id = it->second == ConcreteOutputState::timeline_lane
                        ? [&]() -> std::optional<InternedString> {
                            auto const key = output_identity_key(port, "concrete-output");
                            if (auto const lane_it = concrete_output_external_ids_by_key.find(key);
                                lane_it != concrete_output_external_ids_by_key.end()) {
                                return lane_it->second;
                            }
                            return std::nullopt;
                        }()
                        : std::nullopt,
                });
            }
        } else if (auto const it = logical_output_states_by_key.find(output_state_key);
            it != logical_output_states_by_key.end()) {
            (void)it;
            snapshot.event_output_states.push_back(ProjectSetEventOutputStateRequest{
                .node_id = port.port.logical_node_id,
                .member_ordinal = std::nullopt,
                .output_ordinal = port.port.port_ordinal,
                .state = ProjectEventOutputState::timeline_lane,
                .lane_id = [&]() -> std::optional<InternedString> {
                    auto const key = output_identity_key(port, "logical-output");
                    if (auto const lane_it = logical_output_external_ids_by_key.find(key);
                        lane_it != logical_output_external_ids_by_key.end()) {
                        return lane_it->second;
                    }
                    return std::nullopt;
                }(),
            });
        }
    }

    return snapshot;
}

void GraphInputLanes::handle_task_runner_after_pass(
    TasksRunnerAfterPass const &finished)
{
    std::vector<std::string> instance_ids;
    std::vector<TimelineLaneBatchUpdate> timeline_batches;
    {
        std::scoped_lock lock(mutex);
        current_update_version_index_ = finished.graph_revision + 1;
        instance_ids.assign(
            pending_rebuild_instance_ids.begin(),
            pending_rebuild_instance_ids.end());
        pending_rebuild_instance_ids.clear();
        timeline_batches = take_pending_timeline_batches_locked();
    }
    for (auto const &batch : timeline_batches) {
        apply_timeline_batch(batch);
    }
    if (!timeline_batches.empty() || !instance_ids.empty()) {
        emit_debug_message(
            "graph input lanes pass: revision="
            + std::to_string(finished.graph_revision)
            + " timelineBatches=" + std::to_string(timeline_batches.size())
            + " rebuildInstances=" + std::to_string(instance_ids.size()));
    }
    if (instance_ids.empty()) {
        return;
    }
    std::ranges::sort(instance_ids);
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_graph_input_lanes_rebuild_requested_event,
        GraphInputLanesRebuildRequested{
            .version_index = finished.graph_revision + 1,
            .instance_ids = std::move(instance_ids),
        });
}

GraphInputLanes::BuilderCompletionDiff GraphInputLanes::complete_builder(
    std::string const &instance_id,
    GraphBuilder &builder)
{
    BuilderCompletionDiff diff;
    auto qualify_descriptor = [&](GraphInputPortDescriptor descriptor) {
        return with_runtime_logical_node_id(std::move(descriptor), instance_id);
    };
    auto runtime_node_id = [&](std::string_view logical_node_id) {
        return runtime_logical_node_id(instance_id, logical_node_id);
    };
    auto const logical_sample_inputs = builder.logical_sample_input_families();
    auto const logical_inputs = builder.logical_inputs();
    auto const logical_sample_outputs = builder.logical_sample_output_families();
    auto const logical_outputs = builder.logical_outputs();
    auto const public_sample_inputs = builder.public_sample_input_families();
    auto const public_event_inputs = builder.public_event_inputs();
    auto const public_sample_outputs = builder.public_sample_output_families();
    auto const public_event_outputs = builder.public_event_outputs();
    if (logical_sample_inputs.families.empty() && logical_inputs.event.empty()
        && logical_sample_outputs.families.empty() && logical_outputs.event.empty()
        && public_sample_inputs.families.empty() && public_event_inputs.empty()
        && public_sample_outputs.families.empty() && public_event_outputs.empty()) {
        return diff;
    }
    auto const module_instance_id = module_instance_numeric_id(instance_id);
    emit_debug_message(
        "graph input lanes builder ports: instance=" + instance_id
        + " logicalSampleInputs=" + std::to_string(logical_sample_inputs.families.size())
        + " logicalEventInputs=" + std::to_string(logical_inputs.event.size())
        + " logicalSampleOutputs=" + std::to_string(logical_sample_outputs.families.size())
        + " logicalEventOutputs=" + std::to_string(logical_outputs.event.size())
        + " publicSampleInputs=" + std::to_string(public_sample_inputs.families.size())
        + " publicEventInputs=" + std::to_string(public_event_inputs.size())
        + " publicSampleOutputs=" + std::to_string(public_sample_outputs.families.size())
        + " publicEventOutputs=" + std::to_string(public_event_outputs.size()));

    {
        std::scoped_lock lock(mutex);
        auto &instance_ports = desired_ports_by_instance_id[instance_id];
        instance_ports.clear();

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
            instance_ports.push_back(std::move(desired));
        };

        for (auto const &input : logical_sample_inputs.families) {
            auto logical = qualify_descriptor(sample_port_descriptor(input, std::nullopt));
            auto concrete = qualify_descriptor(sample_port_descriptor(input, input.member_ordinal));
            sample_input_default_values[sample_default_value_key(instance_id, logical)] =
                input.config.default_value;
            sample_input_default_values[sample_default_value_key(instance_id, concrete)] =
                input.config.default_value;
            append_port(std::move(logical));
            append_port(std::move(concrete));
        }
        for (auto const &input : logical_inputs.event) {
            append_port(qualify_descriptor(event_port_descriptor(input, std::nullopt)));
            append_port(qualify_descriptor(event_port_descriptor(input, input.member_ordinal)));
        }

        auto &instance_output_ports = desired_output_ports_by_instance_id[instance_id];
        instance_output_ports.clear();
        std::unordered_set<std::string> appended_outputs;
        auto append_output_port = [&](GraphInputPortDescriptor descriptor) {
            DesiredGraphInputPort desired{
                .instance_id = instance_id,
                .module_instance_id = module_instance_id,
                .port = std::move(descriptor),
            };
            auto const key = desired_port_key(desired);
            if (!appended_outputs.insert(key).second) {
                return;
            }
            instance_output_ports.push_back(std::move(desired));
        };
        for (auto const &output : logical_sample_outputs.families) {
            append_output_port(qualify_descriptor(sample_output_descriptor(output, std::nullopt)));
            append_output_port(
                qualify_descriptor(sample_output_descriptor(output, output.member_ordinal)));
        }
        for (auto const &output : logical_outputs.event) {
            append_output_port(qualify_descriptor(event_output_descriptor(output, std::nullopt)));
            append_output_port(
                qualify_descriptor(event_output_descriptor(output, output.member_ordinal)));
        }

        desired_public_input_ports_by_instance_id[instance_id] =
            public_graph_input_ports_for(instance_id, builder);
        desired_public_output_ports_by_instance_id[instance_id] =
            public_graph_output_ports_for(instance_id, builder);

        refresh_desired_ports_locked();
        refresh_desired_output_ports_locked();
        refresh_desired_public_input_ports_locked();
        refresh_desired_public_output_ports_locked();
        (void)reconcile_ports_locked(&diff.timeline_batch);
        reconcile_output_ports_locked(&diff.timeline_batch);
        reconcile_public_ports_locked(&diff.timeline_batch);
        queue_timeline_batch_locked(diff.timeline_batch);
    }

    std::unordered_map<std::string, size_t> sample_control_node_indices;
    for (auto const &input : logical_sample_inputs.families) {
        auto const qualified_node_id = runtime_node_id(input.logical_node_id);
        auto const concrete_port =
            qualify_descriptor(sample_port_descriptor(input, input.member_ordinal));
        auto const state_key = instance_port_state_key("", concrete_port);
        auto const logical_default_value = live_input_value_or_locked(
            qualified_node_id,
            input.family_ordinal,
            input.config.default_value);
        (void)ensure_live_input_value_initialized_locked(
            qualified_node_id,
            input.family_ordinal,
            input.config.default_value);
        (void)ensure_live_input_value_initialized_locked(
            concrete_key(qualified_node_id, input.member_ordinal),
            input.family_ordinal,
            logical_default_value);

        bool any_existing_connection = false;
        for (auto const &channel : input.channels) {
            any_existing_connection = any_existing_connection || channel.has_existing_connection;
        }

        ConcreteSampleInputState state =
            any_existing_connection
                ? ConcreteSampleInputState::disconnected
                : ConcreteSampleInputState::logical_follow;
        if (auto const it = concrete_sample_input_states_by_key.find(state_key);
            it != concrete_sample_input_states_by_key.end()) {
            state = it->second;
        }

        LaneId prerequisite_lane {};
        for (size_t channel_index = 0; channel_index < input.channels.size(); ++channel_index) {
            auto const &channel = input.channels[channel_index];
            if (!channel.target.has_value()) {
                continue;
            }

            SamplePortRef source;
            bool have_source = false;

            if (state == ConcreteSampleInputState::timeline_lane) {
                auto const bindings = sample_input_bindings(
                    qualified_node_id,
                    input.member_ordinal,
                    input.family_ordinal,
                    input.channel_type);
                if (bindings.sample_inputs.empty()) {
                    throw std::runtime_error("graph input lane completion could not resolve sample input lane");
                }
                prerequisite_lane = bindings.sample_inputs.front().graph_input_lane;
                auto const identity = LaneInputValue::nominal_identity(prerequisite_lane, channel_index);
                if (auto const existing = sample_control_node_indices.find(identity);
                    existing != sample_control_node_indices.end()) {
                    source = SamplePortRef(builder, existing->second, 0);
                } else {
                    source = builder.node<LaneInputValue>(prerequisite_lane, channel_index);
                    sample_control_node_indices.emplace(identity, source.node_index);
                }
                have_source = true;
                builder.mark_runtime_filled_sample_input(*channel.target);
            } else if (state == ConcreteSampleInputState::overridden) {
                auto const key = concrete_key(qualified_node_id, input.member_ordinal);
                source = builder.node<ValueSource>(
                    live_input_value_ptr_or_locked(key, input.family_ordinal));
                have_source = true;
            } else if (state == ConcreteSampleInputState::logical_follow) {
                auto const logical_state_key = graph_input_port_key(
                    qualify_descriptor(sample_port_descriptor(input, std::nullopt)));
                auto const logical_state_it =
                    logical_sample_knob_states_by_key.find(logical_state_key);
                auto const logical_state =
                    logical_state_it != logical_sample_knob_states_by_key.end()
                        ? logical_state_it->second
                        : LogicalSampleKnobState::overridden;

                if (logical_state == LogicalSampleKnobState::timeline_lane) {
                    auto const bindings = sample_input_bindings(
                        qualified_node_id,
                        std::nullopt,
                        input.family_ordinal,
                        input.channel_type);
                    if (bindings.logical_sample_knobs.empty()) {
                        throw std::runtime_error(
                            "logical sample knob timeline lane could not be resolved");
                    }
                    prerequisite_lane = bindings.logical_sample_knobs.front().knob_lane;
                    auto const identity = LaneInputValue::nominal_identity(prerequisite_lane, channel_index);
                    if (auto const existing = sample_control_node_indices.find(identity);
                        existing != sample_control_node_indices.end()) {
                        source = SamplePortRef(builder, existing->second, 0);
                    } else {
                        source = builder.node<LaneInputValue>(prerequisite_lane, channel_index);
                        sample_control_node_indices.emplace(identity, source.node_index);
                    }
                } else {
                    source = builder.node<ValueSource>(
                        live_input_value_ptr_or_locked(qualified_node_id, input.family_ordinal));
                }
                have_source = true;
            } else if (!channel.has_existing_connection) {
                source = builder.node<Constant>(input.config.default_value);
                have_source = true;
            }

            if (have_source) {
                builder.connect_sample_input(*channel.target, source);
            }
        }

        auto first_target = std::find_if(
            input.channels.begin(),
            input.channels.end(),
            [](auto const &channel) { return channel.target.has_value(); });
        if (first_target != input.channels.end()) {
            diff.sample_inputs.push_back(CompletedSampleInput{
                .input = GraphBuilder::VacantSampleInput{
                    .target = *first_target->target,
                    .logical_node_id = input.logical_node_id,
                    .member_ordinal = input.member_ordinal,
                    .config = input.config,
                },
                .lane = prerequisite_lane,
            });
        }
        if (prerequisite_lane) {
            diff.prerequisite_lanes.push_back(prerequisite_lane);
        }
    }

    for (auto const &input : logical_inputs.event) {
        auto const concrete_port =
            qualify_descriptor(event_port_descriptor(input, input.member_ordinal));
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
                ? qualify_descriptor(event_port_descriptor(input, input.member_ordinal))
                : qualify_descriptor(event_port_descriptor(input, std::nullopt));
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

    std::unordered_map<std::uint64_t, NodeRef> sample_sink_by_lane;
    std::unordered_map<std::uint64_t, NodeRef> event_sink_by_lane;

    for (auto const &output : logical_sample_outputs.families) {
        auto const concrete_port =
            qualify_descriptor(sample_output_descriptor(output, output.member_ordinal));
        DesiredGraphInputPort concrete_desired{
            .instance_id = instance_id,
            .module_instance_id = module_instance_id,
            .port = concrete_port,
        };
        auto const state = effective_concrete_output_state_locked(concrete_desired);
        if (!state.has_value()) {
            continue;
        }

        LaneId lane {};
        if (*state == ConcreteOutputState::timeline_lane) {
            lane = graph_output_lane_for(concrete_port, false);
        } else {
            lane = graph_output_lane_for(
                qualify_descriptor(sample_output_descriptor(output, std::nullopt)),
                true);
        }
        if (!lane) {
            continue;
        }
        auto it = sample_sink_by_lane.find(lane.value);
        if (it == sample_sink_by_lane.end()) {
            it = sample_sink_by_lane.emplace(
                lane.value,
                builder.node<GraphSampleOutputSink>(lane, output.channel_type)).first;
        }
        for (size_t channel_index = 0; channel_index < output.channels.size(); ++channel_index) {
            auto const &channel = output.channels[channel_index];
            if (!channel.source.has_value()) {
                continue;
            }
            it->second.connect_input(
                channel_index,
                SamplePortRef(builder, channel.source->node, channel.source->port));
        }
    }

    for (auto const &output : logical_outputs.event) {
        auto const concrete_port =
            qualify_descriptor(event_output_descriptor(output, output.member_ordinal));
        DesiredGraphInputPort concrete_desired{
            .instance_id = instance_id,
            .module_instance_id = module_instance_id,
            .port = concrete_port,
        };
        auto const state = effective_concrete_output_state_locked(concrete_desired);
        if (!state.has_value()) {
            continue;
        }

        LaneId lane {};
        if (*state == ConcreteOutputState::timeline_lane) {
            lane = graph_output_lane_for(concrete_port, false);
        } else {
            lane = graph_output_lane_for(
                qualify_descriptor(event_output_descriptor(output, std::nullopt)),
                true);
        }
        if (!lane) {
            continue;
        }
        auto it = event_sink_by_lane.find(lane.value);
        if (it == event_sink_by_lane.end()) {
            it = event_sink_by_lane.emplace(
                lane.value,
                builder.node<GraphEventOutputSink>(lane, output.config.type)).first;
        }
        it->second.connect_event_input(
            0,
            EventPortRef(builder, output.source.node, output.source.port));
    }

    if (!public_sample_inputs.families.empty()
        || !public_event_inputs.empty()
        || !public_sample_outputs.families.empty()
        || !public_event_outputs.empty()) {
        GraphBuilder execution_builder;
        auto embedded = execution_builder.embed_subgraph(builder);

        {
            std::scoped_lock lock(mutex);
            for (auto const &port : desired_public_input_ports_by_instance_id[instance_id]) {
                auto const logical_state_key = public_sample_input_state_key(
                    port.instance_id, port.source_identity, std::nullopt);
                auto const logical_state_it = public_sample_input_states_by_key.find(logical_state_key);
                auto const logical_state = port.source_identity.empty()
                    ? ProjectPublicSampleInputState::timeline_lane
                    : logical_state_it == public_sample_input_states_by_key.end()
                        ? ProjectPublicSampleInputState::timeline_lane
                        : logical_state_it->second;
                auto const lane = port.port_kind == PortKind::sample
                    ? effective_public_sample_input_lane_locked(port)
                    : std::optional<LaneId>(public_graph_port_lane_for(port));
                auto const use_override_value = port.port_kind == PortKind::sample
                    && !port.source_identity.empty()
                    && logical_state == ProjectPublicSampleInputState::overridden
                    && (!lane.has_value() || !*lane);
                if ((!lane.has_value() || !*lane) && !use_override_value) {
                    continue;
                }
                if (port.port_kind == PortKind::sample) {
                    for (size_t channel_index = 0; channel_index < port.channels.size(); ++channel_index) {
                        auto const port_ordinal = port.channels[channel_index].port_ordinal;
                        if (!port_ordinal.has_value()) {
                            continue;
                        }
                        auto source = use_override_value
                            ? static_cast<NodeRef>(execution_builder.node<ValueSource>(
                                &ensure_public_sample_input_value_locked(
                                    port.instance_id, port.source_identity, port.default_value)))
                            : execution_builder.node<LaneInputValue>(*lane, channel_index);
                        embedded.connect_input(*port_ordinal, source);
                    }
                } else if (port.event_type.has_value()) {
                    embedded.connect_event_input(
                        port.port_ordinal,
                        execution_builder.node<LaneInputEvents>(*lane, *port.event_type).event_port(0));
                }
                if (lane.has_value() && *lane) {
                    diff.prerequisite_lanes.push_back(*lane);
                }
            }

            for (auto const &port : desired_public_output_ports_by_instance_id[instance_id]) {
                auto const lane = public_graph_port_lane_for(port);
                if (!lane) {
                    continue;
                }
                if (port.port_kind == PortKind::sample) {
                    auto sink = execution_builder.node<GraphSampleOutputSink>(
                        lane,
                        port.sample_channel_type.value_or(ChannelTypeId::mono));
                    for (size_t channel_index = 0; channel_index < port.channels.size(); ++channel_index) {
                        auto const port_ordinal = port.channels[channel_index].port_ordinal;
                        if (!port_ordinal.has_value()) {
                            continue;
                        }
                        sink.connect_input(channel_index, embedded[*port_ordinal]);
                    }
                } else if (port.event_type.has_value()) {
                    execution_builder.node<GraphEventOutputSink>(lane, *port.event_type)
                        .connect_event_input(0, embedded.event_port(port.port_ordinal));
                }
            }
        }

        execution_builder.outputs();
        builder = std::move(execution_builder);
    }

    std::ranges::sort(diff.prerequisite_lanes, {}, &LaneId::value);
    diff.prerequisite_lanes.erase(
        std::unique(diff.prerequisite_lanes.begin(), diff.prerequisite_lanes.end()),
        diff.prerequisite_lanes.end());
    return diff;
}

void GraphInputLanes::handle_sample_block_published(
    LaneId lane,
    BorrowedSampleBlock const &block)
{
    publish_sample_output_block(lane, block);
}

void GraphInputLanes::handle_event_block_published(
    LaneId lane,
    std::span<TimedEvent const> events)
{
    publish_event_output_block(lane, events);
}

OwnedSampleBlock GraphInputLanes::handle_sample_block_requested(LaneId lane) const
{
    return sample_output_block(lane);
}

std::vector<TimedEvent> GraphInputLanes::handle_event_block_requested(LaneId lane) const
{
    return event_output_block(lane);
}
} // namespace iv
