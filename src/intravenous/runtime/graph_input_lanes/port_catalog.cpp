#include <intravenous/runtime/graph_input_lanes/port_catalog.h>

#include <intravenous/graph/names.h>

#include <ranges>
#include <charconv>

namespace iv {
namespace {
std::string runtime_virtual_node_id(std::string_view instance_id, std::string_view virtual_node_id)
{
    return std::string(instance_id) + "\x1fvirtual:" + std::string(virtual_node_id);
}

int module_instance_numeric_id(std::string_view instance_id)
{
    auto const separator = instance_id.rfind(':');
    if (separator != std::string_view::npos) {
        int parsed = 0;
        auto const numeric = instance_id.substr(separator + 1);
        auto const [ptr, ec] = std::from_chars(numeric.data(), numeric.data() + numeric.size(), parsed);
        if (ec == std::errc{} && ptr == numeric.data() + numeric.size()) {
            return parsed;
        }
    }
    return static_cast<int>(std::hash<std::string>{}(std::string(instance_id)));
}

void append_descriptors(
    std::vector<GraphInputLanes::DesiredGraphPort>& ports,
    std::string const& instance_id,
    int module_instance_id,
    std::string const& virtual_node_id,
    PortKind port_kind,
    std::span<IntrospectionPortInfo const> virtual_ports,
    std::optional<size_t> member_ordinal = std::nullopt)
{
    ports.reserve(ports.size() + virtual_ports.size());
    for (auto const& port : virtual_ports) {
        ports.push_back(GraphInputLanes::DesiredGraphPort{
            .instance_id = instance_id,
            .module_instance_id = module_instance_id,
            .port = GraphInputPortDescriptor{
                .virtual_node_id = virtual_node_id,
                .node_bundle_port_ordinal = member_ordinal,
                .port_kind = port_kind,
                .port_ordinal = port.ordinal,
                .port_name = port.name,
                .port_type = port.type,
                .sample_channel_type = port.sample_channel_type,
            },
            .authored_connected = port.connectivity != VirtualPortConnectivity::disconnected,
            .default_value = port.default_value,
            .min = port.min,
            .max = port.max,
        });
    }
}
} // namespace

auto GraphInputLanesPortCatalog::graph_inputs(IvModuleInstance const& instance)
    -> std::vector<GraphInputLanes::DesiredGraphPort>
{
    std::vector<GraphInputLanes::DesiredGraphPort> ports;
    auto const module_instance_id = module_instance_numeric_id(instance.instance_id);
    for (auto const& node : instance.introspection.virtual_nodes) {
        auto const node_id = runtime_virtual_node_id(instance.instance_id, node.id);
        append_descriptors(ports, instance.instance_id, module_instance_id, node_id, PortKind::sample, node.sample_inputs);
        append_descriptors(ports, instance.instance_id, module_instance_id, node_id, PortKind::event, node.event_inputs);
        for (auto const& member : node.members) {
            append_descriptors(ports, instance.instance_id, module_instance_id, node_id,
                               PortKind::sample, member.sample_inputs, member.ordinal);
            append_descriptors(ports, instance.instance_id, module_instance_id, node_id,
                               PortKind::event, member.event_inputs, member.ordinal);
        }
    }
    return ports;
}

auto GraphInputLanesPortCatalog::graph_outputs(IvModuleInstance const& instance)
    -> std::vector<GraphInputLanes::DesiredGraphPort>
{
    std::vector<GraphInputLanes::DesiredGraphPort> ports;
    auto const module_instance_id = module_instance_numeric_id(instance.instance_id);
    for (auto const& node : instance.introspection.virtual_nodes) {
        auto const node_id = runtime_virtual_node_id(instance.instance_id, node.id);
        append_descriptors(ports, instance.instance_id, module_instance_id, node_id, PortKind::sample, node.sample_outputs);
        append_descriptors(ports, instance.instance_id, module_instance_id, node_id, PortKind::event, node.event_outputs);
        for (auto const& member : node.members) {
            append_descriptors(ports, instance.instance_id, module_instance_id, node_id,
                               PortKind::sample, member.sample_outputs, member.ordinal);
            append_descriptors(ports, instance.instance_id, module_instance_id, node_id,
                               PortKind::event, member.event_outputs, member.ordinal);
        }
    }
    return ports;
}

auto GraphInputLanesPortCatalog::public_inputs(
    IvModuleInstance const& instance)
    -> std::vector<GraphInputLanes::DesiredPublicGraphPort>
{
    std::vector<GraphInputLanes::DesiredPublicGraphPort> ports;
    auto const& instance_id = instance.instance_id;
    auto const module_instance_id = module_instance_numeric_id(instance_id);
    for (auto const& family : instance.introspection.public_sample_inputs) {
        std::vector<GraphInputLanes::DesiredPublicGraphPortChannel> channels;
        channels.reserve(family.channels.size());
        for (auto const& channel : family.channels) {
            channels.push_back({.port_ordinal = channel.port_ordinals.empty() ? std::nullopt : std::optional<size_t>(channel.port_ordinals.front())});
        }
        ports.push_back({
            .instance_id = instance_id, .module_instance_id = module_instance_id, .input = true,
            .port_kind = PortKind::sample, .port_ordinal = family.family_ordinal,
            .port_name = family.family_name, .port_type = "sample", .sample_channel_type = family.channel_type,
            .default_value = family.input_config.default_value, .min = family.input_config.min, .max = family.input_config.max,
            .source_infos = family.source_infos,
            .source_identity = family.source_infos.empty() ? std::string{} : family.source_infos.front().declaration_identity,
            .graph_connected = family.authored_connected,
            .channels = std::move(channels),
        });
    }
    for (auto const& input : instance.introspection.public_event_inputs) {
        ports.push_back({
            .instance_id = instance_id, .module_instance_id = module_instance_id, .input = true,
            .port_kind = PortKind::event, .port_ordinal = input.port_ordinal, .port_name = input.config.name,
            .port_type = details::event_type_name(input.config.type), .event_type = input.config.type,
            .source_infos = input.source_infos,
            .source_identity = input.source_infos.empty() ? std::string{} : input.source_infos.front().declaration_identity,
            .graph_connected = input.graph_connected,
        });
    }
    return ports;
}

auto GraphInputLanesPortCatalog::public_outputs(
    IvModuleInstance const& instance)
    -> std::vector<GraphInputLanes::DesiredPublicGraphPort>
{
    std::vector<GraphInputLanes::DesiredPublicGraphPort> ports;
    auto const& instance_id = instance.instance_id;
    auto const module_instance_id = module_instance_numeric_id(instance_id);
    for (auto const& family : instance.introspection.public_sample_outputs) {
        std::vector<GraphInputLanes::DesiredPublicGraphPortChannel> channels;
        channels.reserve(family.channels.size());
        for (auto const& channel : family.channels) {
            channels.push_back({.port_ordinal = channel.port_ordinals.empty() ? std::nullopt : std::optional<size_t>(channel.port_ordinals.front())});
        }
        bool emitted_annotated_contributor = false;
        for (size_t channel_ordinal = 0; channel_ordinal < family.channels.size(); ++channel_ordinal) {
            for (auto const& source : family.channels[channel_ordinal].source_infos) {
                emitted_annotated_contributor = true;
                ports.push_back({
                    .instance_id = instance_id, .module_instance_id = module_instance_id, .input = false,
                    .port_kind = PortKind::sample, .port_ordinal = family.family_ordinal,
                    .port_name = family.family_name, .port_type = "sample", .sample_channel_type = family.channel_type,
                    .source_infos = {source}, .source_identity = source.declaration_identity,
                    .node_bundle_port_ordinal = channel_ordinal, .channels = channels,
                });
            }
        }
        if (!emitted_annotated_contributor) {
            ports.push_back({
                .instance_id = instance_id, .module_instance_id = module_instance_id, .input = false,
                .port_kind = PortKind::sample, .port_ordinal = family.family_ordinal,
                .port_name = family.family_name, .port_type = "sample", .sample_channel_type = family.channel_type,
                .channels = std::move(channels),
            });
        }
    }
    for (auto const& output : instance.introspection.public_event_outputs) {
        ports.push_back({
            .instance_id = instance_id, .module_instance_id = module_instance_id, .input = false,
            .port_kind = PortKind::event, .port_ordinal = output.port_ordinal, .port_name = output.config.name,
            .port_type = details::event_type_name(output.config.type), .event_type = output.config.type,
            .source_infos = output.source_infos,
            .source_identity = output.source_infos.empty() ? std::string{} : output.source_infos.front().declaration_identity,
        });
    }
    return ports;
}
} // namespace iv
