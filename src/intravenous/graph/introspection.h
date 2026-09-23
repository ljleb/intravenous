#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/builder/constexpr_hash.hpp>
#include <intravenous/graph/configured_graph.hpp>

#include <algorithm>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace iv {
namespace details {

constexpr std::string introspection_decimal_string(std::size_t value)
{
    char digits[std::numeric_limits<std::size_t>::digits10 + 2] {};
    std::size_t begin = sizeof(digits);
    do {
        digits[--begin] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    return std::string(digits + begin, digits + sizeof(digits));
}

constexpr void sort_and_deduplicate_introspection_spans(
    std::vector<SourceSpan>& spans)
{
    std::sort(spans.begin(), spans.end(), [](auto const& left, auto const& right) {
        return std::tie(left.file_path, left.begin, left.end)
            < std::tie(right.file_path, right.begin, right.end);
    });
    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
}

constexpr std::vector<SourceSpan> introspection_source_spans_for(
    std::span<SourceInfo const> infos)
{
    std::vector<SourceSpan> spans;
    spans.reserve(infos.size());
    for (auto const& info : infos) {
        if (info.span.file_path.empty() || info.span.begin > info.span.end) {
            continue;
        }
        spans.push_back(info.span);
    }
    sort_and_deduplicate_introspection_spans(spans);
    return spans;
}

struct IntrospectionSampleChannelIdHash {
    template<class ChannelId>
    constexpr std::size_t operator()(ChannelId const& value) const
    {
        auto result = constexpr_hash_combine(0, value.bundle);
        result = constexpr_hash_combine(result, value.port);
        return constexpr_hash_combine(result, value.channel);
    }
};

struct IntrospectionEventPortIdHash {
    template<class PortId>
    constexpr std::size_t operator()(PortId const& value) const
    {
        return constexpr_hash_combine(value.bundle, value.port);
    }
};

struct ConfiguredGraphConnectivity {
    ConstexprHashSet<SampleInputChannelId, IntrospectionSampleChannelIdHash>
        sample_inputs;
    ConstexprHashSet<SampleOutputChannelId, IntrospectionSampleChannelIdHash>
        sample_outputs;
    ConstexprHashSet<EventInputPortId, IntrospectionEventPortIdHash>
        event_inputs;
    ConstexprHashSet<EventOutputPortId, IntrospectionEventPortIdHash>
        event_outputs;
};

constexpr ConfiguredGraphConnectivity configured_graph_connectivity(
    GraphBuilderConnections const& connections)
{
    ConfiguredGraphConnectivity result;
    for (auto const& connection : connections.configured_sample_connections()) {
        result.sample_inputs.insert_range(
            connection.target_channels.begin(), connection.target_channels.end());
        result.sample_outputs.insert_range(
            connection.source_channels.begin(), connection.source_channels.end());
    }
    for (auto const& connection : connections.configured_event_connections()) {
        result.event_inputs.insert_range(
            connection.targets.begin(), connection.targets.end());
        result.event_outputs.insert_range(
            connection.sources.begin(), connection.sources.end());
    }
    return result;
}

constexpr bool introspection_sample_channel_is_connected(
    ConfiguredGraphConnectivity const& connectivity,
    SampleInputChannelId channel)
{
    return connectivity.sample_inputs.contains(channel);
}

constexpr bool introspection_sample_channel_is_connected(
    ConfiguredGraphConnectivity const& connectivity,
    SampleOutputChannelId channel)
{
    return connectivity.sample_outputs.contains(channel);
}

template<class Mapping>
constexpr std::vector<IntrospectionPortInfo> project_introspection_sample_ports(
    std::vector<Mapping> const& mappings,
    GraphBuilderNodeBundles const& node_bundles,
    ConfiguredGraphConnectivity const& connectivity,
    bool inputs)
{
    std::vector<IntrospectionPortInfo> result;
    result.reserve(mappings.size());
    for (auto const& mapping : mappings) {
        if (mapping.channels.empty()) continue;

        auto const first_channel = mapping.channels.front();
        NodeBundlePortId const first_address{
            first_channel.bundle, PortKind::sample, first_channel.port};
        Sample default_value = 0.0f;
        std::optional<Sample> min;
        std::optional<Sample> max;
        std::size_t history = 0;
        std::size_t latency = 0;
        if (inputs) {
            auto const config =
                node_bundles.resolve_sample_input(first_address).config;
            default_value = config.default_value;
            min = config.min;
            max = config.max;
            history = realtime_history_or_zero(config);
        } else {
            latency = realtime_latency_or_zero(
                node_bundles.resolve_sample_output(first_address).config);
        }

        bool any_connected = false;
        bool any_disconnected = false;
        for (auto const channel : mapping.channels) {
            auto const connected = introspection_sample_channel_is_connected(
                connectivity, channel);
            any_connected = any_connected || connected;
            any_disconnected = any_disconnected || !connected;
        }
        result.push_back(IntrospectionPortInfo{
            .name = mapping.name,
            .type = "sample",
            .connectivity = any_connected && any_disconnected
                ? VirtualPortConnectivity::mixed
                : any_connected ? VirtualPortConnectivity::connected
                                : VirtualPortConnectivity::disconnected,
            .ordinal = mapping.ordinal,
            .default_value = default_value,
            .min = min,
            .max = max,
            .history = history,
            .latency = latency,
            .sample_channel_type = mapping.channel_layout.channel_type,
            .source_spans =
                introspection_source_spans_for(mapping.source_infos),
        });
    }
    return result;
}

template<class Mapping>
constexpr std::vector<IntrospectionPortInfo>
project_bundle_introspection_sample_ports(
    std::vector<Mapping> const& mappings,
    GraphBuilderNodeBundles const& node_bundles,
    ConfiguredGraphConnectivity const& connectivity,
    NodeBundleHandle bundle_handle,
    bool inputs)
{
    std::vector<Mapping> bundle_mappings;
    bundle_mappings.reserve(mappings.size());
    for (auto const& mapping : mappings) {
        auto projected = mapping;
        std::erase_if(projected.channels, [&](auto const channel) {
            return channel.bundle != bundle_handle;
        });
        if (!projected.channels.empty()) {
            bundle_mappings.push_back(std::move(projected));
        }
    }
    return project_introspection_sample_ports(
        bundle_mappings, node_bundles, connectivity, inputs);
}

constexpr std::vector<IntrospectionPortInfo> project_introspection_event_ports(
    std::vector<VirtualEventPortMapping> const& mappings,
    ConfiguredGraphConnectivity const& connectivity,
    bool inputs)
{
    std::vector<IntrospectionPortInfo> result;
    result.reserve(mappings.size());
    for (auto const& mapping : mappings) {
        if (mapping.node_bundle_ports.empty()) continue;
        bool connected = false;
        for (auto const port : mapping.node_bundle_ports) {
            connected = connected || (inputs
                ? connectivity.event_inputs.contains(EventInputPortId{
                    port.node_bundle_handle, port.port_ordinal})
                : connectivity.event_outputs.contains(EventOutputPortId{
                    port.node_bundle_handle, port.port_ordinal}));
        }
        result.push_back(IntrospectionPortInfo{
            .name = mapping.name,
            .type = event_type_name(mapping.type),
            .connectivity = connected ? VirtualPortConnectivity::connected
                                      : VirtualPortConnectivity::disconnected,
            .ordinal = mapping.ordinal,
            .source_spans =
                introspection_source_spans_for(mapping.source_infos),
        });
    }
    return result;
}

constexpr std::vector<IntrospectionPortInfo>
project_bundle_introspection_event_ports(
    std::vector<VirtualEventPortMapping> const& mappings,
    ConfiguredGraphConnectivity const& connectivity,
    NodeBundleHandle bundle_handle,
    bool inputs)
{
    auto projected = mappings;
    for (auto& mapping : projected) {
        std::erase_if(mapping.node_bundle_ports, [&](auto const port) {
            return port.node_bundle_handle != bundle_handle;
        });
    }
    std::erase_if(projected, [](auto const& mapping) {
        return mapping.node_bundle_ports.empty();
    });
    return project_introspection_event_ports(projected, connectivity, inputs);
}

constexpr void append_configured_virtual_node_metadata(
    GraphIntrospectionMetadata& metadata,
    GraphBuilderNodeBundles const& node_bundles,
    GraphBuilderVirtualNodes const& virtual_nodes,
    ConfiguredGraphConnectivity const& connectivity)
{
    metadata.virtual_nodes.reserve(virtual_nodes.records().size());
    for (auto const& record : virtual_nodes.records()) {
        auto const node_index = metadata.virtual_nodes.size();
        metadata.virtual_nodes.push_back(IntrospectionVirtualNode{
            .id = record.id,
            .source_identity = record.source_identity,
            .type_identity = record.type_identity,
            .source_spans = introspection_source_spans_for(record.source_infos),
            .sample_inputs = project_introspection_sample_ports(
                record.sample_inputs, node_bundles, connectivity, true),
            .sample_outputs = project_introspection_sample_ports(
                record.sample_outputs, node_bundles, connectivity, false),
            .event_inputs = project_introspection_event_ports(
                record.event_inputs, connectivity, true),
            .event_outputs = project_introspection_event_ports(
                record.event_outputs, connectivity, false),
        });
        auto& node = metadata.virtual_nodes[node_index];

        if (record.type_identity == "sample-port"
            || record.type_identity == "event-port") {
            node.kind = record.type_identity == "sample-port"
                ? "Sample port"
                : "Event port";
            auto const backing_id = record.type_identity + ":" + record.id;
            node.backing_node_ids.push_back(backing_id);
            node.members.push_back(IntrospectionVirtualNode::Member{
                .ordinal = 0,
                .backing_node_id = backing_id,
                .kind = node.kind,
                .type_identity = record.type_identity,
                .sample_inputs = node.sample_inputs,
                .sample_outputs = node.sample_outputs,
                .event_inputs = node.event_inputs,
                .event_outputs = node.event_outputs,
            });
            continue;
        }

        for (std::size_t bundle_ordinal = 0;
             bundle_ordinal < record.node_bundle_handles.size();
             ++bundle_ordinal) {
            auto const handle = record.node_bundle_handles[bundle_ordinal];
            auto const& bundle = node_bundles.bundle(handle);
            auto const type = std::string(bundle.type_identity());
            if (node.kind.empty()) node.kind = type;
            if (node.type_identity.empty()) node.type_identity = type;
            auto const backing_id = "node-bundle:" + record.id + ":"
                + introspection_decimal_string(bundle_ordinal);
            node.backing_node_ids.push_back(backing_id);
            node.members.push_back(IntrospectionVirtualNode::Member{
                .ordinal = bundle_ordinal,
                .backing_node_id = backing_id,
                .kind = type,
                .type_identity = type,
                .sample_inputs = project_bundle_introspection_sample_ports(
                    record.sample_inputs, node_bundles, connectivity, handle,
                    true),
                .sample_outputs = project_bundle_introspection_sample_ports(
                    record.sample_outputs, node_bundles, connectivity, handle,
                    false),
                .event_inputs = project_bundle_introspection_event_ports(
                    record.event_inputs, connectivity, handle, true),
                .event_outputs = project_bundle_introspection_event_ports(
                    record.event_outputs, connectivity, handle, false),
            });
        }
    }
}

} // namespace details

constexpr GraphIntrospectionMetadata build_graph_introspection_metadata(
    ConfiguredGraph const& configured)
{
    auto const connectivity =
        details::configured_graph_connectivity(configured.connections);
    GraphIntrospectionMetadata metadata;
    details::append_configured_virtual_node_metadata(
        metadata, configured.node_bundles, configured.virtual_nodes,
        connectivity);

    auto sample_inputs =
        configured.public_ports.sample_input_families(configured.node_bundles);
    for (auto& family : sample_inputs.families) {
        family.configured_connected = std::ranges::any_of(
            family.channels, [&](auto const& channel) {
                return std::ranges::any_of(
                    channel.port_ordinals, [&](auto const ordinal) {
                        auto const channels =
                            configured.node_bundles.sample_output_channels({
                                configured.public_ports.boundary_handle(),
                                PortKind::sample,
                                ordinal,
                            });
                        return std::ranges::any_of(
                            channels, [&](auto const source) {
                                return connectivity.sample_outputs.contains(
                                    source);
                            });
                    });
            });
    }
    metadata.public_sample_inputs = std::move(sample_inputs.families);

    metadata.public_event_inputs =
        configured.public_ports.collected_event_inputs(configured.node_bundles);
    for (auto& input : metadata.public_event_inputs) {
        auto const ports = configured.node_bundles.event_output_ports({
            configured.public_ports.boundary_handle(),
            PortKind::event,
            input.port_ordinal,
        });
        input.graph_connected = std::ranges::any_of(
            ports, [&](auto const source) {
                return connectivity.event_outputs.contains(source);
            });
    }

    metadata.public_sample_outputs =
        configured.public_ports.sample_output_families(configured.node_bundles)
            .families;
    metadata.public_event_outputs =
        configured.public_ports.collected_event_outputs(
            configured.node_bundles);
    return metadata;
}

} // namespace iv
