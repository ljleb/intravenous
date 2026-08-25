#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/static_storage.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace iv {
struct StaticSourceSpan {
    StaticString file_path {};
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    SourceSpan value() const
    {
        return {
            .file_path = std::string(file_path.view()),
            .begin = begin,
            .end = end,
        };
    }
};

struct StaticSourceInfo {
    StaticString declaration_identity {};
    StaticSourceSpan span {};

    SourceInfo value() const
    {
        return {
            .declaration_identity = std::string(declaration_identity.view()),
            .span = span.value(),
        };
    }
};

struct StaticIntrospectionPortInfo {
    StaticString name {};
    StaticString type {};
    VirtualPortConnectivity connectivity = VirtualPortConnectivity::disconnected;
    size_t ordinal = 0;
    Sample default_value = 0.0f;
    bool has_min = false;
    Sample min = 0.0f;
    bool has_max = false;
    Sample max = 0.0f;
    size_t history = 0;
    size_t latency = 0;
    bool has_sample_channel_type = false;
    ChannelTypeId sample_channel_type = ChannelTypeId::mono;

    IntrospectionPortInfo value() const
    {
        return {
            .name = std::string(name.view()),
            .type = std::string(type.view()),
            .connectivity = connectivity,
            .ordinal = ordinal,
            .default_value = default_value,
            .min = has_min ? std::optional<Sample>{min} : std::nullopt,
            .max = has_max ? std::optional<Sample>{max} : std::nullopt,
            .history = history,
            .latency = latency,
            .sample_channel_type = has_sample_channel_type
                ? std::optional<ChannelTypeId>{sample_channel_type}
                : std::nullopt,
        };
    }
};

struct StaticIntrospectionVirtualNodeMember {
    size_t ordinal = 0;
    StaticString backing_node_id {};
    StaticString kind {};
    StaticString type_identity {};
    StaticSpan<StaticIntrospectionPortInfo> sample_inputs {};
    StaticSpan<StaticIntrospectionPortInfo> sample_outputs {};
    StaticSpan<StaticIntrospectionPortInfo> event_inputs {};
    StaticSpan<StaticIntrospectionPortInfo> event_outputs {};

    IntrospectionVirtualNode::Member value() const;
};

struct StaticIntrospectionVirtualNode {
    StaticString id {};
    StaticString kind {};
    StaticString source_identity {};
    StaticString type_identity {};
    StaticSpan<StaticSourceSpan> source_spans {};
    StaticSpan<StaticIntrospectionPortInfo> sample_inputs {};
    StaticSpan<StaticIntrospectionPortInfo> sample_outputs {};
    StaticSpan<StaticIntrospectionPortInfo> event_inputs {};
    StaticSpan<StaticIntrospectionPortInfo> event_outputs {};
    StaticSpan<StaticString> backing_node_ids {};
    StaticSpan<StaticIntrospectionVirtualNodeMember> members {};

    IntrospectionVirtualNode value() const;
};

struct StaticGraphBuilderPublicSamplePortChannel {
    StaticSpan<size_t> port_ordinals {};
    StaticSpan<StaticSourceInfo> source_infos {};

    GraphBuilderPublicSamplePortChannel value() const;
};

struct StaticGraphBuilderPublicSamplePortFamily {
    size_t family_ordinal = 0;
    StaticString family_name {};
    StaticInputConfig input_config {};
    StaticOutputConfig output_config {};
    ChannelTypeId channel_type = ChannelTypeId::mono;
    StaticSpan<StaticGraphBuilderPublicSamplePortChannel> channels {};
    StaticSpan<StaticSourceInfo> source_infos {};
    bool authored_connected = false;

    GraphBuilderPublicSamplePortFamily value() const;
};

struct StaticGraphBuilderPublicEventInput {
    size_t port_ordinal = 0;
    StaticEventInputConfig config {};
    StaticSpan<StaticSourceInfo> source_infos {};
    bool graph_connected = false;

    GraphBuilderPublicEventInput value() const;
};

struct StaticGraphBuilderPublicEventOutput {
    size_t port_ordinal = 0;
    StaticEventOutputConfig config {};
    StaticSpan<StaticSourceInfo> source_infos {};

    GraphBuilderPublicEventOutput value() const;
};

struct StaticGraphIntrospectionMetadata {
    StaticSpan<StaticIntrospectionVirtualNode> virtual_nodes {};
    StaticSpan<StaticGraphBuilderPublicSamplePortFamily> public_sample_inputs {};
    StaticSpan<StaticGraphBuilderPublicEventInput> public_event_inputs {};
    StaticSpan<StaticGraphBuilderPublicSamplePortFamily> public_sample_outputs {};
    StaticSpan<StaticGraphBuilderPublicEventOutput> public_event_outputs {};

    GraphIntrospectionMetadata metadata() const;
};

namespace details {
template<class StaticValue, class Value, class Freeze>
consteval StaticSpan<StaticValue> define_static_values(
    std::vector<Value> const& values,
    Freeze&& freeze)
{
    std::vector<StaticValue> frozen;
    frozen.reserve(values.size());
    for (auto const& value : values)
        frozen.push_back(freeze(value));
    return define_static_span(frozen);
}

consteval StaticSourceSpan define_static_source_span(SourceSpan const& span)
{
    return {
        .file_path = define_static_string(span.file_path),
        .begin = span.begin,
        .end = span.end,
    };
}

consteval StaticSourceInfo define_static_source_info(SourceInfo const& info)
{
    return {
        .declaration_identity = define_static_string(info.declaration_identity),
        .span = define_static_source_span(info.span),
    };
}

consteval StaticSpan<StaticSourceSpan> define_static_source_spans(
    std::vector<SourceSpan> const& spans)
{
    return define_static_values<StaticSourceSpan>(
        spans, [](auto const& span) { return define_static_source_span(span); });
}

consteval StaticSpan<StaticSourceInfo> define_static_source_infos(
    std::vector<SourceInfo> const& infos)
{
    return define_static_values<StaticSourceInfo>(
        infos, [](auto const& info) { return define_static_source_info(info); });
}

consteval StaticIntrospectionPortInfo define_static_introspection_port(
    IntrospectionPortInfo const& port)
{
    return {
        .name = define_static_string(port.name),
        .type = define_static_string(port.type),
        .connectivity = port.connectivity,
        .ordinal = port.ordinal,
        .default_value = port.default_value,
        .has_min = port.min.has_value(),
        .min = port.min.value_or(Sample{0}),
        .has_max = port.max.has_value(),
        .max = port.max.value_or(Sample{0}),
        .history = port.history,
        .latency = port.latency,
        .has_sample_channel_type = port.sample_channel_type.has_value(),
        .sample_channel_type = port.sample_channel_type.value_or(ChannelTypeId::mono),
    };
}

consteval StaticSpan<StaticIntrospectionPortInfo> define_static_introspection_ports(
    std::vector<IntrospectionPortInfo> const& ports)
{
    return define_static_values<StaticIntrospectionPortInfo>(
        ports, [](auto const& port) { return define_static_introspection_port(port); });
}

consteval StaticSpan<StaticString> define_static_strings(
    std::vector<std::string> const& strings)
{
    return define_static_values<StaticString>(
        strings, [](auto const& string) { return define_static_string(string); });
}

consteval StaticIntrospectionVirtualNodeMember define_static_virtual_member(
    IntrospectionVirtualNode::Member const& member)
{
    return {
        .ordinal = member.ordinal,
        .backing_node_id = define_static_string(member.backing_node_id),
        .kind = define_static_string(member.kind),
        .type_identity = define_static_string(member.type_identity),
        .sample_inputs = define_static_introspection_ports(member.sample_inputs),
        .sample_outputs = define_static_introspection_ports(member.sample_outputs),
        .event_inputs = define_static_introspection_ports(member.event_inputs),
        .event_outputs = define_static_introspection_ports(member.event_outputs),
    };
}

consteval StaticIntrospectionVirtualNode define_static_virtual_node(
    IntrospectionVirtualNode const& node)
{
    return {
        .id = define_static_string(node.id),
        .kind = define_static_string(node.kind),
        .source_identity = define_static_string(node.source_identity),
        .type_identity = define_static_string(node.type_identity),
        .source_spans = define_static_source_spans(node.source_spans),
        .sample_inputs = define_static_introspection_ports(node.sample_inputs),
        .sample_outputs = define_static_introspection_ports(node.sample_outputs),
        .event_inputs = define_static_introspection_ports(node.event_inputs),
        .event_outputs = define_static_introspection_ports(node.event_outputs),
        .backing_node_ids = define_static_strings(node.backing_node_ids),
        .members = define_static_values<StaticIntrospectionVirtualNodeMember>(
            node.members,
            [](auto const& member) { return define_static_virtual_member(member); }),
    };
}

consteval StaticGraphBuilderPublicSamplePortChannel define_static_public_sample_channel(
    GraphBuilderPublicSamplePortChannel const& channel)
{
    return {
        .port_ordinals = define_static_span(channel.port_ordinals),
        .source_infos = define_static_source_infos(channel.source_infos),
    };
}

consteval StaticGraphBuilderPublicSamplePortFamily define_static_public_sample_family(
    GraphBuilderPublicSamplePortFamily const& family)
{
    return {
        .family_ordinal = family.family_ordinal,
        .family_name = define_static_string(family.family_name),
        .input_config = define_static_config(family.input_config),
        .output_config = define_static_config(family.output_config),
        .channel_type = family.channel_type,
        .channels = define_static_values<StaticGraphBuilderPublicSamplePortChannel>(
            family.channels,
            [](auto const& channel) {
                return define_static_public_sample_channel(channel);
            }),
        .source_infos = define_static_source_infos(family.source_infos),
        .authored_connected = family.authored_connected,
    };
}

consteval StaticGraphBuilderPublicEventInput define_static_public_event_input(
    GraphBuilderPublicEventInput const& input)
{
    return {
        .port_ordinal = input.port_ordinal,
        .config = define_static_config(input.config),
        .source_infos = define_static_source_infos(input.source_infos),
        .graph_connected = input.graph_connected,
    };
}

consteval StaticGraphBuilderPublicEventOutput define_static_public_event_output(
    GraphBuilderPublicEventOutput const& output)
{
    return {
        .port_ordinal = output.port_ordinal,
        .config = define_static_config(output.config),
        .source_infos = define_static_source_infos(output.source_infos),
    };
}

consteval StaticGraphIntrospectionMetadata define_static_metadata(
    GraphIntrospectionMetadata const& metadata)
{
    return {
        .virtual_nodes = define_static_values<StaticIntrospectionVirtualNode>(
            metadata.virtual_nodes,
            [](auto const& node) { return define_static_virtual_node(node); }),
        .public_sample_inputs =
            define_static_values<StaticGraphBuilderPublicSamplePortFamily>(
                metadata.public_sample_inputs,
                [](auto const& family) {
                    return define_static_public_sample_family(family);
                }),
        .public_event_inputs =
            define_static_values<StaticGraphBuilderPublicEventInput>(
                metadata.public_event_inputs,
                [](auto const& input) {
                    return define_static_public_event_input(input);
                }),
        .public_sample_outputs =
            define_static_values<StaticGraphBuilderPublicSamplePortFamily>(
                metadata.public_sample_outputs,
                [](auto const& family) {
                    return define_static_public_sample_family(family);
                }),
        .public_event_outputs =
            define_static_values<StaticGraphBuilderPublicEventOutput>(
                metadata.public_event_outputs,
                [](auto const& output) {
                    return define_static_public_event_output(output);
                }),
    };
}

template<class StaticValue, class Value, class Thaw>
std::vector<Value> copy_static_values(StaticSpan<StaticValue> values, Thaw&& thaw)
{
    std::vector<Value> result;
    result.reserve(values.size);
    for (auto const& value : values)
        result.push_back(thaw(value));
    return result;
}
} // namespace details

inline IntrospectionVirtualNode::Member
StaticIntrospectionVirtualNodeMember::value() const
{
    return {
        .ordinal = ordinal,
        .backing_node_id = std::string(backing_node_id.view()),
        .kind = std::string(kind.view()),
        .type_identity = std::string(type_identity.view()),
        .sample_inputs = details::copy_static_values<
            StaticIntrospectionPortInfo, IntrospectionPortInfo>(
                sample_inputs, [](auto const& port) { return port.value(); }),
        .sample_outputs = details::copy_static_values<
            StaticIntrospectionPortInfo, IntrospectionPortInfo>(
                sample_outputs, [](auto const& port) { return port.value(); }),
        .event_inputs = details::copy_static_values<
            StaticIntrospectionPortInfo, IntrospectionPortInfo>(
                event_inputs, [](auto const& port) { return port.value(); }),
        .event_outputs = details::copy_static_values<
            StaticIntrospectionPortInfo, IntrospectionPortInfo>(
                event_outputs, [](auto const& port) { return port.value(); }),
    };
}

inline IntrospectionVirtualNode StaticIntrospectionVirtualNode::value() const
{
    return {
        .id = std::string(id.view()),
        .kind = std::string(kind.view()),
        .source_identity = std::string(source_identity.view()),
        .type_identity = std::string(type_identity.view()),
        .source_spans = details::copy_static_values<StaticSourceSpan, SourceSpan>(
            source_spans, [](auto const& span) { return span.value(); }),
        .sample_inputs = details::copy_static_values<
            StaticIntrospectionPortInfo, IntrospectionPortInfo>(
                sample_inputs, [](auto const& port) { return port.value(); }),
        .sample_outputs = details::copy_static_values<
            StaticIntrospectionPortInfo, IntrospectionPortInfo>(
                sample_outputs, [](auto const& port) { return port.value(); }),
        .event_inputs = details::copy_static_values<
            StaticIntrospectionPortInfo, IntrospectionPortInfo>(
                event_inputs, [](auto const& port) { return port.value(); }),
        .event_outputs = details::copy_static_values<
            StaticIntrospectionPortInfo, IntrospectionPortInfo>(
                event_outputs, [](auto const& port) { return port.value(); }),
        .backing_node_ids = details::copy_static_values<StaticString, std::string>(
            backing_node_ids,
            [](auto const& string) { return std::string(string.view()); }),
        .members = details::copy_static_values<
            StaticIntrospectionVirtualNodeMember, IntrospectionVirtualNode::Member>(
                members, [](auto const& member) { return member.value(); }),
    };
}

inline GraphBuilderPublicSamplePortChannel
StaticGraphBuilderPublicSamplePortChannel::value() const
{
    return {
        .port_ordinals = std::vector<size_t>(
            port_ordinals.begin(), port_ordinals.end()),
        .source_infos = details::copy_static_values<StaticSourceInfo, SourceInfo>(
            source_infos, [](auto const& info) { return info.value(); }),
    };
}

inline GraphBuilderPublicSamplePortFamily
StaticGraphBuilderPublicSamplePortFamily::value() const
{
    return {
        .family_ordinal = family_ordinal,
        .family_name = std::string(family_name.view()),
        .input_config = input_config.config(),
        .output_config = output_config.config(),
        .channel_type = channel_type,
        .channels = details::copy_static_values<
            StaticGraphBuilderPublicSamplePortChannel,
            GraphBuilderPublicSamplePortChannel>(
                channels, [](auto const& channel) { return channel.value(); }),
        .source_infos = details::copy_static_values<StaticSourceInfo, SourceInfo>(
            source_infos, [](auto const& info) { return info.value(); }),
        .authored_connected = authored_connected,
    };
}

inline GraphBuilderPublicEventInput
StaticGraphBuilderPublicEventInput::value() const
{
    return {
        .port_ordinal = port_ordinal,
        .config = config.config(),
        .source_infos = details::copy_static_values<StaticSourceInfo, SourceInfo>(
            source_infos, [](auto const& info) { return info.value(); }),
        .graph_connected = graph_connected,
    };
}

inline GraphBuilderPublicEventOutput
StaticGraphBuilderPublicEventOutput::value() const
{
    return {
        .port_ordinal = port_ordinal,
        .config = config.config(),
        .source_infos = details::copy_static_values<StaticSourceInfo, SourceInfo>(
            source_infos, [](auto const& info) { return info.value(); }),
    };
}

inline GraphIntrospectionMetadata StaticGraphIntrospectionMetadata::metadata() const
{
    return {
        .virtual_nodes = details::copy_static_values<
            StaticIntrospectionVirtualNode, IntrospectionVirtualNode>(
                virtual_nodes, [](auto const& node) { return node.value(); }),
        .public_sample_inputs = details::copy_static_values<
            StaticGraphBuilderPublicSamplePortFamily,
            GraphBuilderPublicSamplePortFamily>(
                public_sample_inputs,
                [](auto const& family) { return family.value(); }),
        .public_event_inputs = details::copy_static_values<
            StaticGraphBuilderPublicEventInput, GraphBuilderPublicEventInput>(
                public_event_inputs,
                [](auto const& input) { return input.value(); }),
        .public_sample_outputs = details::copy_static_values<
            StaticGraphBuilderPublicSamplePortFamily,
            GraphBuilderPublicSamplePortFamily>(
                public_sample_outputs,
                [](auto const& family) { return family.value(); }),
        .public_event_outputs = details::copy_static_values<
            StaticGraphBuilderPublicEventOutput, GraphBuilderPublicEventOutput>(
                public_event_outputs,
                [](auto const& output) { return output.value(); }),
    };
}
} // namespace iv
