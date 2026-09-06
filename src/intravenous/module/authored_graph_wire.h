#pragma once

#include <intravenous/graph/authored_graph.hpp>
#include <intravenous/graph/reflected_node.hpp>
#include <intravenous/module/abi.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iv {

struct AuthoredNodeConfigBytes {
    std::vector<std::byte> bytes{};
    std::size_t alignment = 1;
};

struct SerializedAuthoredGraph {
    std::string json{};
    std::vector<AuthoredNodeConfigBytes> node_configs{};
};

namespace wire_details {
using json = nlohmann::json;

template<class Enum>
json enum_json(Enum value)
{
    return static_cast<std::underlying_type_t<Enum>>(value);
}

template<class Enum>
Enum enum_from_json(json const& value)
{
    return static_cast<Enum>(value.get<std::underlying_type_t<Enum>>());
}

inline json source_info(SourceInfo const& value)
{
    return {
        {"identity", value.declaration_identity},
        {"file", value.span.file_path},
        {"begin", value.span.begin},
        {"end", value.span.end},
    };
}

inline SourceInfo source_info(json const& value)
{
    return {
        .declaration_identity = value.at("identity").get<std::string>(),
        .span = {
            .file_path = value.at("file").get<std::string>(),
            .begin = value.at("begin").get<std::uint32_t>(),
            .end = value.at("end").get<std::uint32_t>(),
        },
    };
}

inline json source_infos(std::span<SourceInfo const> values)
{
    json result = json::array();
    for (auto const& value : values) result.push_back(source_info(value));
    return result;
}

inline std::vector<SourceInfo> source_infos(json const& values)
{
    std::vector<SourceInfo> result;
    result.reserve(values.size());
    for (auto const& value : values) result.push_back(source_info(value));
    return result;
}

inline json source_info_groups(std::vector<std::vector<SourceInfo>> const& groups)
{
    json result = json::array();
    for (auto const& group : groups) result.push_back(source_infos(group));
    return result;
}

inline std::vector<std::vector<SourceInfo>> source_info_groups(json const& groups)
{
    std::vector<std::vector<SourceInfo>> result;
    result.reserve(groups.size());
    for (auto const& group : groups) result.push_back(source_infos(group));
    return result;
}

inline json channel_layout(ChannelLayout const& value)
{
    return {
        {"channel_type", enum_json(value.channel_type)},
        {"sample_layout", enum_json(value.sample_layout)},
    };
}

inline ChannelLayout channel_layout(json const& value)
{
    return {
        .channel_type = enum_from_json<ChannelTypeId>(value.at("channel_type")),
        .sample_layout = enum_from_json<SampleStreamLayout>(value.at("sample_layout")),
    };
}

inline json input_config(InputConfig const& value)
{
    return {
        {"name", value.name},
        {"layout", channel_layout(value.channel_layout)},
        {"history", value.history},
        {"default", value.default_value.value},
        {"min", value.min.value},
        {"max", value.max.value},
    };
}

inline InputConfig input_config(json const& value)
{
    return {
        .name = value.at("name").get<std::string>(),
        .channel_layout = channel_layout(value.at("layout")),
        .history = value.at("history").get<std::size_t>(),
        .default_value = Sample{value.at("default").get<float>()},
        .min = Sample{value.at("min").get<float>()},
        .max = Sample{value.at("max").get<float>()},
    };
}

inline json output_config(OutputConfig const& value)
{
    return {
        {"name", value.name},
        {"layout", channel_layout(value.channel_layout)},
        {"latency", value.latency},
        {"history", value.history},
    };
}

inline OutputConfig output_config(json const& value)
{
    return {
        .name = value.at("name").get<std::string>(),
        .channel_layout = channel_layout(value.at("layout")),
        .latency = value.at("latency").get<std::size_t>(),
        .history = value.at("history").get<std::size_t>(),
    };
}

inline json event_input_config(EventInputConfig const& value)
{
    return {{"name", value.name}, {"type", enum_json(value.type)}};
}

inline EventInputConfig event_input_config(json const& value)
{
    return {
        .name = value.at("name").get<std::string>(),
        .type = enum_from_json<EventTypeId>(value.at("type")),
    };
}

inline json event_output_config(EventOutputConfig const& value)
{
    return {{"name", value.name}, {"type", enum_json(value.type)}};
}

inline EventOutputConfig event_output_config(json const& value)
{
    return {
        .name = value.at("name").get<std::string>(),
        .type = enum_from_json<EventTypeId>(value.at("type")),
    };
}

template<class T, class Fn>
json config_array(std::span<T const> values, Fn&& fn)
{
    json result = json::array();
    for (auto const& value : values) result.push_back(fn(value));
    return result;
}

template<class T, class Fn>
std::vector<T> config_array(json const& values, Fn&& fn)
{
    std::vector<T> result;
    result.reserve(values.size());
    for (auto const& value : values) result.push_back(fn(value));
    return result;
}

inline json node_ports(NodePorts const& value)
{
    return {
        {"sample_inputs", config_array<InputConfig>(value.sample_inputs, input_config)},
        {"sample_outputs", config_array<OutputConfig>(value.sample_outputs, output_config)},
        {"event_inputs", config_array<EventInputConfig>(value.event_input_configs, event_input_config)},
        {"event_outputs", config_array<EventOutputConfig>(value.event_output_configs, event_output_config)},
    };
}

inline NodePorts node_ports(json const& value)
{
    return {
        .sample_inputs = config_array<InputConfig>(value.at("sample_inputs"), input_config),
        .sample_outputs = config_array<OutputConfig>(value.at("sample_outputs"), output_config),
        .event_input_configs = config_array<EventInputConfig>(value.at("event_inputs"), event_input_config),
        .event_output_configs = config_array<EventOutputConfig>(value.at("event_outputs"), event_output_config),
    };
}

inline json code_key(NodeCodeKey value)
{
    return {{"low", value.low}, {"high", value.high}};
}

inline NodeCodeKey code_key(json const& value)
{
    return {
        .low = value.at("low").get<std::uint64_t>(),
        .high = value.at("high").get<std::uint64_t>(),
    };
}

inline json node_bundle_port(NodeBundlePortId value)
{
    return {
        {"bundle", value.node_bundle_handle},
        {"kind", enum_json(value.port_kind)},
        {"ordinal", value.port_ordinal},
    };
}

inline NodeBundlePortId node_bundle_port(json const& value)
{
    return {
        .node_bundle_handle = value.at("bundle").get<std::size_t>(),
        .port_kind = enum_from_json<PortKind>(value.at("kind")),
        .port_ordinal = value.at("ordinal").get<std::size_t>(),
    };
}

inline json sample_output_channel(SampleOutputChannelId value)
{
    return {{"bundle", value.bundle}, {"port", value.port}, {"channel", value.channel}};
}
inline SampleOutputChannelId sample_output_channel(json const& value)
{
    return {
        .bundle = value.at("bundle").get<std::size_t>(),
        .port = value.at("port").get<std::size_t>(),
        .channel = value.at("channel").get<std::size_t>(),
    };
}
inline json sample_input_channel(SampleInputChannelId value)
{
    return {{"bundle", value.bundle}, {"port", value.port}, {"channel", value.channel}};
}
inline SampleInputChannelId sample_input_channel(json const& value)
{
    return {
        .bundle = value.at("bundle").get<std::size_t>(),
        .port = value.at("port").get<std::size_t>(),
        .channel = value.at("channel").get<std::size_t>(),
    };
}
inline json event_output_port(EventOutputPortId value)
{
    return {{"bundle", value.bundle}, {"port", value.port}};
}
inline EventOutputPortId event_output_port(json const& value)
{
    return {.bundle = value.at("bundle").get<std::size_t>(), .port = value.at("port").get<std::size_t>()};
}
inline json event_input_port(EventInputPortId value)
{
    return {{"bundle", value.bundle}, {"port", value.port}};
}
inline EventInputPortId event_input_port(json const& value)
{
    return {.bundle = value.at("bundle").get<std::size_t>(), .port = value.at("port").get<std::size_t>()};
}

template<class T, class Fn>
json simple_array(std::span<T const> values, Fn&& fn)
{
    json result = json::array();
    for (auto const& value : values) result.push_back(fn(value));
    return result;
}

template<class T, class Fn>
std::vector<T> simple_array(json const& values, Fn&& fn)
{
    std::vector<T> result;
    result.reserve(values.size());
    for (auto const& value : values) result.push_back(fn(value));
    return result;
}

inline json state_structure(NodeStateStructure const& value)
{
    json fields = json::array();
    for (auto const& field : value.fields) {
        json item{
            {"name", field.name},
            {"type", field.type_name},
            {"bit_offset", field.bit_offset},
            {"size_bits", field.size_bits},
            {"alignment_bits", field.alignment_bits},
        };
        if (field.bit_width) item["bit_width"] = *field.bit_width;
        fields.push_back(std::move(item));
    }
    return {
        {"size_bits", value.size_bits},
        {"alignment_bits", value.alignment_bits},
        {"fields", std::move(fields)},
    };
}

inline NodeStateStructure state_structure(json const& value)
{
    NodeStateStructure result{
        .size_bits = value.at("size_bits").get<std::size_t>(),
        .alignment_bits = value.at("alignment_bits").get<std::size_t>(),
    };
    for (auto const& field : value.at("fields")) {
        NodeStateFieldStructure item{
            .name = field.at("name").get<std::string>(),
            .type_name = field.at("type").get<std::string>(),
            .bit_offset = field.at("bit_offset").get<std::size_t>(),
            .size_bits = field.at("size_bits").get<std::size_t>(),
            .alignment_bits = field.at("alignment_bits").get<std::size_t>(),
        };
        if (field.contains("bit_width")) item.bit_width = field.at("bit_width").get<std::size_t>();
        result.fields.push_back(std::move(item));
    }
    return result;
}

inline json virtual_sample_mapping(VirtualSampleInputPortMapping const& value)
{
    json members = json::array();
    for (auto const& member : value.member_channels)
        members.push_back(simple_array<SampleInputChannelId>(member, sample_input_channel));
    return {
        {"name", value.name}, {"ordinal", value.ordinal},
        {"layout", channel_layout(value.channel_layout)},
        {"channels", simple_array<SampleInputChannelId>(value.channels, sample_input_channel)},
        {"members", std::move(members)},
    };
}

inline VirtualSampleInputPortMapping virtual_sample_input_mapping(json const& value)
{
    VirtualSampleInputPortMapping result{
        .name = value.at("name").get<std::string>(),
        .ordinal = value.at("ordinal").get<std::size_t>(),
        .channel_layout = channel_layout(value.at("layout")),
        .channels = simple_array<SampleInputChannelId>(value.at("channels"), sample_input_channel),
    };
    for (auto const& member : value.at("members"))
        result.member_channels.push_back(simple_array<SampleInputChannelId>(member, sample_input_channel));
    return result;
}

inline json virtual_sample_mapping(VirtualSampleOutputPortMapping const& value)
{
    json members = json::array();
    for (auto const& member : value.member_channels)
        members.push_back(simple_array<SampleOutputChannelId>(member, sample_output_channel));
    return {
        {"name", value.name}, {"ordinal", value.ordinal},
        {"layout", channel_layout(value.channel_layout)},
        {"channels", simple_array<SampleOutputChannelId>(value.channels, sample_output_channel)},
        {"members", std::move(members)},
    };
}

inline VirtualSampleOutputPortMapping virtual_sample_output_mapping(json const& value)
{
    VirtualSampleOutputPortMapping result{
        .name = value.at("name").get<std::string>(),
        .ordinal = value.at("ordinal").get<std::size_t>(),
        .channel_layout = channel_layout(value.at("layout")),
        .channels = simple_array<SampleOutputChannelId>(value.at("channels"), sample_output_channel),
    };
    for (auto const& member : value.at("members"))
        result.member_channels.push_back(simple_array<SampleOutputChannelId>(member, sample_output_channel));
    return result;
}

inline json virtual_event_mapping(VirtualEventPortMapping const& value)
{
    return {
        {"name", value.name}, {"ordinal", value.ordinal}, {"type", enum_json(value.type)},
        {"ports", simple_array<NodeBundlePortId>(value.node_bundle_ports, node_bundle_port)},
    };
}

inline VirtualEventPortMapping virtual_event_mapping(json const& value)
{
    return {
        .name = value.at("name").get<std::string>(),
        .ordinal = value.at("ordinal").get<std::size_t>(),
        .type = enum_from_json<EventTypeId>(value.at("type")),
        .node_bundle_ports = simple_array<NodeBundlePortId>(value.at("ports"), node_bundle_port),
    };
}

inline json virtual_node(VirtualNodeRecord const& value)
{
    json inputs = json::array();
    for (auto const& item : value.sample_inputs) inputs.push_back(virtual_sample_mapping(item));
    json outputs = json::array();
    for (auto const& item : value.sample_outputs) outputs.push_back(virtual_sample_mapping(item));
    json event_inputs = json::array();
    for (auto const& item : value.event_inputs) event_inputs.push_back(virtual_event_mapping(item));
    json event_outputs = json::array();
    for (auto const& item : value.event_outputs) event_outputs.push_back(virtual_event_mapping(item));
    return {
        {"id", value.id}, {"source_identity", value.source_identity},
        {"type_identity", value.type_identity},
        {"source_infos", source_infos(value.source_infos)},
        {"bundles", value.node_bundle_handles},
        {"sample_inputs", std::move(inputs)},
        {"sample_outputs", std::move(outputs)},
        {"event_inputs", std::move(event_inputs)},
        {"event_outputs", std::move(event_outputs)},
    };
}

inline VirtualNodeRecord virtual_node(json const& value)
{
    VirtualNodeRecord result{
        .id = value.at("id").get<std::string>(),
        .source_identity = value.at("source_identity").get<std::string>(),
        .type_identity = value.at("type_identity").get<std::string>(),
        .source_infos = source_infos(value.at("source_infos")),
        .node_bundle_handles = value.at("bundles").get<std::vector<std::size_t>>(),
    };
    for (auto const& item : value.at("sample_inputs")) result.sample_inputs.push_back(virtual_sample_input_mapping(item));
    for (auto const& item : value.at("sample_outputs")) result.sample_outputs.push_back(virtual_sample_output_mapping(item));
    for (auto const& item : value.at("event_inputs")) result.event_inputs.push_back(virtual_event_mapping(item));
    for (auto const& item : value.at("event_outputs")) result.event_outputs.push_back(virtual_event_mapping(item));
    return result;
}

inline details::NodeCompilerRecord const& find_type(
    std::span<details::NodeCompilerRecord const> types, NodeCodeKey key)
{
    auto const it = std::find_if(types.begin(), types.end(), [&](auto const& value) {
        return value.code_key == key;
    });
    if (it == types.end()) throw std::runtime_error("authored graph references an unknown node code key");
    return *it;
}

} // namespace wire_details

inline SerializedAuthoredGraph serialize_authored_graph(
    AuthoredGraph const& authored,
    std::span<std::pair<NodeCodeKey, NodeStateStructure> const> state_structures = {})
{
    using namespace wire_details;
    json root;
    root["version"] = 1;
    root["identity"] = authored.identity.value;

    SerializedAuthoredGraph result;
    json bundles = json::array();
    std::size_t config_ordinal = 0;
    authored.node_bundles.for_each_authored_bundle([&](AuthoredNodeBundleView view) {
        json item{
            {"kind", enum_json(view.kind)},
            {"virtual_nodes", std::vector<std::size_t>(view.virtual_node_handles.begin(), view.virtual_node_handles.end())},
            {"source_infos", source_infos(view.source_infos)},
        };
        if (view.kind == AuthoredNodeBundleKind::concrete) {
            if (!view.node_storage || !*view.node_storage || !view.code_key || !view.ports)
                throw std::runtime_error("concrete authored node has incomplete compiler storage");
            item["ports"] = node_ports(*view.ports);
            item["code_key"] = code_key(*view.code_key);
            item["config_ordinal"] = config_ordinal++;
            item["node_size"] = view.node_size;
            item["node_alignment"] = view.node_alignment;
            if (view.lifetime && view.lifetime->ttl_samples) item["ttl"] = *view.lifetime->ttl_samples;
            item["type_identity"] = view.type_identity ? *view.type_identity : std::string{};
            item["reflected_type_name"] = view.reflected_type_name ? *view.reflected_type_name : std::string{};
            item["internal_latency"] = view.internal_latency_samples;
            item["maximum_block_size"] = view.maximum_block_size;
            if (view.default_ttl_samples && *view.default_ttl_samples)
                item["default_ttl"] = **view.default_ttl_samples;
            item["block_skippable"] = view.block_skippable;
            if (view.static_sample_value && *view.static_sample_value)
                item["static_sample_value"] = (**view.static_sample_value).value;
            if (view.deferred_detach && *view.deferred_detach) {
                item["deferred_detach"] = {
                    {"kind", enum_json((*view.deferred_detach)->kind)},
                    {"id", (*view.deferred_detach)->id},
                    {"loop_extra_latency", (*view.deferred_detach)->loop_extra_latency},
                };
            }
            for (auto const& [key, structure] : state_structures) {
                if (key == *view.code_key) {
                    item["state_structure"] = state_structure(structure);
                    break;
                }
            }
            AuthoredNodeConfigBytes config;
            config.alignment = view.node_alignment;
            config.bytes.resize(view.node_size);
            std::memcpy(config.bytes.data(), (*view.node_storage).get(), view.node_size);
            result.node_configs.push_back(std::move(config));
        } else if (view.kind == AuthoredNodeBundleKind::tiled) {
            item["tiled_members"] = std::vector<std::size_t>(view.tiled_members.begin(), view.tiled_members.end());
            item["type_identity"] = view.type_identity ? *view.type_identity : std::string{};
            item["sample_inputs"] = config_array<InputConfig>(view.sample_input_configs, input_config);
            item["sample_outputs"] = config_array<OutputConfig>(view.sample_output_configs, output_config);
            item["event_inputs"] = config_array<EventInputConfig>(view.event_input_configs, event_input_config);
            item["event_outputs"] = config_array<EventOutputConfig>(view.event_output_configs, event_output_config);
        } else if (view.kind == AuthoredNodeBundleKind::boundary) {
            item["sample_inputs"] = config_array<InputConfig>(view.sample_input_configs, input_config);
            item["sample_outputs"] = config_array<OutputConfig>(view.sample_output_configs, output_config);
            item["event_inputs"] = config_array<EventInputConfig>(view.event_input_configs, event_input_config);
            item["event_outputs"] = config_array<EventOutputConfig>(view.event_output_configs, event_output_config);
        } else {
            item["boundary"] = view.subgraph_boundary;
            item["child_begin"] = view.subgraph_child_begin;
            item["child_count"] = view.subgraph_child_count;
            item["subgraph_kind"] = view.subgraph_kind ? *view.subgraph_kind : std::string{};
            if (view.lifetime && view.lifetime->ttl_samples) item["ttl"] = *view.lifetime->ttl_samples;
            item["type_identity"] = view.type_identity ? *view.type_identity : std::string{};
            item["sample_input_count"] = view.subgraph_sample_input_count;
            item["sample_output_count"] = view.subgraph_sample_output_count;
            item["event_input_count"] = view.subgraph_event_input_count;
            item["event_output_count"] = view.subgraph_event_output_count;
        }
        bundles.push_back(std::move(item));
    });
    root["bundles"] = std::move(bundles);

    json sample_connections = json::array();
    for (auto const& connection : authored.connections.authored_sample_connections()) {
        sample_connections.push_back({
            {"source_type", enum_json(connection.source_type)},
            {"source_channels", simple_array<SampleOutputChannelId>(connection.source_channels, sample_output_channel)},
            {"target_type", enum_json(connection.target_type)},
            {"target_channels", simple_array<SampleInputChannelId>(connection.target_channels, sample_input_channel)},
        });
    }
    root["sample_connections"] = std::move(sample_connections);

    json event_connections = json::array();
    for (auto const& connection : authored.connections.authored_event_connections()) {
        event_connections.push_back({
            {"source_type", enum_json(connection.source_type)},
            {"sources", simple_array<EventOutputPortId>(connection.sources, event_output_port)},
            {"target_type", enum_json(connection.target_type)},
            {"targets", simple_array<EventInputPortId>(connection.targets, event_input_port)},
        });
    }
    root["event_connections"] = std::move(event_connections);

    auto const public_ports = authored.public_ports.authored_record();
    json public_json{
        {"boundary", public_ports.boundary},
        {"sample_input_source_infos", source_info_groups(public_ports.sample_input_source_infos)},
        {"event_input_source_infos", source_info_groups(public_ports.event_input_source_infos)},
        {"last_sample_output_port_ordinals", public_ports.last_sample_output_port_ordinals},
        {"sample_output_source_infos", source_info_groups(public_ports.sample_output_source_infos)},
        {"event_output_source_infos", source_info_groups(public_ports.event_output_source_infos)},
        {"sample_outputs_defined", public_ports.sample_outputs_defined},
    };
    json members = json::array();
    for (auto const& member : public_ports.sample_output_members) {
        members.push_back({
            {"family_name", member.family_name},
            {"channel_type", enum_json(member.channel_type)},
            {"channel_index", member.channel_index},
            {"whole_stream", member.whole_stream},
        });
    }
    public_json["sample_output_members"] = std::move(members);
    root["public_ports"] = std::move(public_json);

    root["next_detach_id"] = authored.detach.next_detach_id();
    json detached = json::array();
    for (auto const& info : authored.detach.authored_infos()) {
        detached.push_back({
            {"detach_id", info.detach_id},
            {"source_type", enum_json(info.source_type)},
            {"source_channels", simple_array<SampleOutputChannelId>(info.source_channels, sample_output_channel)},
            {"writer_bundle", info.writer_bundle},
            {"reader_bundle", info.reader_bundle},
            {"reader_channel", sample_output_channel(info.reader_channel)},
            {"loop_extra_latency", info.loop_extra_latency},
        });
    }
    root["detached"] = std::move(detached);

    json virtual_nodes = json::array();
    for (auto const& record : authored.virtual_nodes.records()) virtual_nodes.push_back(virtual_node(record));
    root["virtual_nodes"] = std::move(virtual_nodes);

    result.json = root.dump();
    return result;
}

inline AuthoredGraph deserialize_authored_graph(
    std::string_view text,
    std::span<details::NodeCompilerRecord const> node_types,
    std::span<ModuleNodeConfigRecord const> node_configs)
{
    using namespace wire_details;
    auto root = json::parse(text.begin(), text.end());
    if (root.at("version").get<unsigned>() != 1)
        throw std::runtime_error("unsupported authored graph wire version");

    std::vector<AuthoredNodeBundleRecord> bundles;
    bundles.reserve(root.at("bundles").size());
    for (auto const& item : root.at("bundles")) {
        AuthoredNodeBundleRecord record;
        record.kind = enum_from_json<AuthoredNodeBundleKind>(item.at("kind"));
        record.virtual_node_handles = item.at("virtual_nodes").get<std::vector<std::size_t>>();
        record.source_infos = source_infos(item.at("source_infos"));
        if (record.kind == AuthoredNodeBundleKind::concrete) {
            record.ports = node_ports(item.at("ports"));
            record.code_key = code_key(item.at("code_key"));
            auto const config_ordinal = item.at("config_ordinal").get<std::size_t>();
            if (config_ordinal >= node_configs.size())
                throw std::runtime_error("authored graph config ordinal is out of range");
            auto const& config = node_configs[config_ordinal];
            record.node_size = item.at("node_size").get<std::size_t>();
            record.node_alignment = item.at("node_alignment").get<std::size_t>();
            if (config.size != record.node_size || config.alignment < record.node_alignment)
                throw std::runtime_error("module node config layout does not match authored graph");
            record.node_storage = std::shared_ptr<void const>(config.data, [](void const*) {});
            auto const& type = find_type(node_types, record.code_key);
            record.operations = {.runtime = type.runtime};
            record.operations.runtime.node_data = config.data;
            if (item.contains("state_structure")) {
                record.state_structure_storage = std::make_shared<NodeStateStructure>(
                    state_structure(item.at("state_structure")));
                record.operations.runtime.state_structure = record.state_structure_storage.get();
            }
            if (item.contains("ttl")) record.lifetime.ttl_samples = item.at("ttl").get<std::size_t>();
            record.type_identity = item.at("type_identity").get<std::string>();
            record.reflected_type_name = item.at("reflected_type_name").get<std::string>();
            record.internal_latency_samples = item.at("internal_latency").get<std::size_t>();
            record.maximum_block_size = item.at("maximum_block_size").get<std::size_t>();
            if (item.contains("default_ttl")) record.default_ttl_samples = item.at("default_ttl").get<std::size_t>();
            record.block_skippable = item.at("block_skippable").get<bool>();
            if (item.contains("static_sample_value")) record.static_sample_value = Sample{item.at("static_sample_value").get<float>()};
            if (item.contains("deferred_detach")) {
                auto const& value = item.at("deferred_detach");
                record.deferred_detach = DeferredDetachNode{
                    .kind = enum_from_json<DeferredDetachNodeKind>(value.at("kind")),
                    .id = value.at("id").get<std::size_t>(),
                    .loop_extra_latency = value.at("loop_extra_latency").get<std::size_t>(),
                };
            }
        } else if (record.kind == AuthoredNodeBundleKind::tiled) {
            record.tiled_members = item.at("tiled_members").get<std::vector<std::size_t>>();
            record.type_identity = item.at("type_identity").get<std::string>();
            record.sample_input_configs = config_array<InputConfig>(item.at("sample_inputs"), input_config);
            record.sample_output_configs = config_array<OutputConfig>(item.at("sample_outputs"), output_config);
            record.event_input_configs = config_array<EventInputConfig>(item.at("event_inputs"), event_input_config);
            record.event_output_configs = config_array<EventOutputConfig>(item.at("event_outputs"), event_output_config);
        } else if (record.kind == AuthoredNodeBundleKind::boundary) {
            record.sample_input_configs = config_array<InputConfig>(item.at("sample_inputs"), input_config);
            record.sample_output_configs = config_array<OutputConfig>(item.at("sample_outputs"), output_config);
            record.event_input_configs = config_array<EventInputConfig>(item.at("event_inputs"), event_input_config);
            record.event_output_configs = config_array<EventOutputConfig>(item.at("event_outputs"), event_output_config);
        } else {
            record.subgraph_boundary = item.at("boundary").get<std::size_t>();
            record.subgraph_child_begin = item.at("child_begin").get<std::size_t>();
            record.subgraph_child_count = item.at("child_count").get<std::size_t>();
            record.subgraph_kind = item.at("subgraph_kind").get<std::string>();
            if (item.contains("ttl")) record.lifetime.ttl_samples = item.at("ttl").get<std::size_t>();
            record.type_identity = item.at("type_identity").get<std::string>();
            record.subgraph_sample_input_count = item.at("sample_input_count").get<std::size_t>();
            record.subgraph_sample_output_count = item.at("sample_output_count").get<std::size_t>();
            record.subgraph_event_input_count = item.at("event_input_count").get<std::size_t>();
            record.subgraph_event_output_count = item.at("event_output_count").get<std::size_t>();
        }
        bundles.push_back(std::move(record));
    }

    std::vector<AuthoredSampleConnection> sample_connections;
    for (auto const& value : root.at("sample_connections")) {
        sample_connections.push_back({
            .source_type = enum_from_json<ChannelTypeId>(value.at("source_type")),
            .source_channels = simple_array<SampleOutputChannelId>(value.at("source_channels"), sample_output_channel),
            .target_type = enum_from_json<ChannelTypeId>(value.at("target_type")),
            .target_channels = simple_array<SampleInputChannelId>(value.at("target_channels"), sample_input_channel),
        });
    }
    std::vector<AuthoredEventConnection> event_connections;
    for (auto const& value : root.at("event_connections")) {
        event_connections.push_back({
            .source_type = enum_from_json<EventTypeId>(value.at("source_type")),
            .sources = simple_array<EventOutputPortId>(value.at("sources"), event_output_port),
            .target_type = enum_from_json<EventTypeId>(value.at("target_type")),
            .targets = simple_array<EventInputPortId>(value.at("targets"), event_input_port),
        });
    }

    auto const& public_value = root.at("public_ports");
    AuthoredPublicPortsRecord public_ports{
        .boundary = public_value.at("boundary").get<std::size_t>(),
        .sample_input_source_infos = source_info_groups(public_value.at("sample_input_source_infos")),
        .event_input_source_infos = source_info_groups(public_value.at("event_input_source_infos")),
        .last_sample_output_port_ordinals = public_value.at("last_sample_output_port_ordinals").get<std::vector<std::size_t>>(),
        .sample_output_source_infos = source_info_groups(public_value.at("sample_output_source_infos")),
        .event_output_source_infos = source_info_groups(public_value.at("event_output_source_infos")),
        .sample_outputs_defined = public_value.at("sample_outputs_defined").get<bool>(),
    };
    for (auto const& member : public_value.at("sample_output_members")) {
        public_ports.sample_output_members.push_back({
            .family_name = member.at("family_name").get<std::string>(),
            .channel_type = enum_from_json<ChannelTypeId>(member.at("channel_type")),
            .channel_index = member.at("channel_index").get<std::size_t>(),
            .whole_stream = member.at("whole_stream").get<bool>(),
        });
    }

    std::vector<AuthoredDetachedSamplePortInfo> detached;
    for (auto const& value : root.at("detached")) {
        detached.push_back({
            .detach_id = value.at("detach_id").get<std::size_t>(),
            .source_type = enum_from_json<ChannelTypeId>(value.at("source_type")),
            .source_channels = simple_array<SampleOutputChannelId>(value.at("source_channels"), sample_output_channel),
            .writer_bundle = value.at("writer_bundle").get<std::size_t>(),
            .reader_bundle = value.at("reader_bundle").get<std::size_t>(),
            .reader_channel = sample_output_channel(value.at("reader_channel")),
            .loop_extra_latency = value.at("loop_extra_latency").get<std::size_t>(),
        });
    }

    std::vector<VirtualNodeRecord> virtual_nodes;
    for (auto const& value : root.at("virtual_nodes")) virtual_nodes.push_back(virtual_node(value));

    return {
        .identity = GraphBuilderIdentity{root.at("identity").get<std::string>()},
        .node_bundles = GraphBuilderNodeBundles::from_authored_records(bundles),
        .connections = GraphBuilderConnections::from_authored_connections(sample_connections, event_connections),
        .public_ports = GraphBuilderPublicPorts::from_authored_record(public_ports),
        .detach = GraphBuilderDetach::from_authored_infos(root.at("next_detach_id").get<std::size_t>(), detached),
        .annotations = {},
        .virtual_nodes = GraphBuilderVirtualNodes::from_authored_records(virtual_nodes),
    };
}

} // namespace iv
