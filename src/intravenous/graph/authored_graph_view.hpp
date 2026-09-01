#pragma once

#include <intravenous/graph/authored_graph.hpp>
#include <intravenous/graph/static_storage.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iv {
// All strings and variable-size ranges in this view point into static storage
// owned by the authoring DSO.  The host copies this view into AuthoredGraph
// before lowering, while retaining the DSO for opaque node operations.
struct StaticSourceInfo {
    StaticString declaration_identity {};
    StaticString file_path {};
    uint32_t begin = 0;
    uint32_t end = 0;
};

struct StaticNodePorts {
    StaticSpan<StaticInputConfig> sample_inputs {};
    StaticSpan<StaticOutputConfig> sample_outputs {};
    StaticSpan<StaticEventInputConfig> event_inputs {};
    StaticSpan<StaticEventOutputConfig> event_outputs {};
};

struct StaticDeferredDetachNode {
    DeferredDetachNodeKind kind = DeferredDetachNodeKind::writer;
    size_t id = 0;
    size_t loop_extra_latency = 1;
};

struct StaticAuthoredNodeBundle {
    AuthoredNodeBundleKind kind = AuthoredNodeBundleKind::boundary;
    StaticNodePorts ports {};
    ReflectedNodeOperations operations {};
    bool has_ttl_samples = false;
    size_t ttl_samples = 0;
    StaticString type_identity {};
    StaticString reflected_type_name {};
    size_t internal_latency_samples = 0;
    size_t maximum_block_size = MAX_BLOCK_SIZE;
    bool has_default_ttl_samples = false;
    size_t default_ttl_samples = 0;
    bool block_skippable = false;
    bool has_static_sample_value = false;
    Sample static_sample_value = 0.0f;
    bool has_deferred_detach = false;
    StaticDeferredDetachNode deferred_detach {};

    StaticSpan<size_t> tiled_members {};
    StaticSpan<StaticInputConfig> sample_input_configs {};
    StaticSpan<StaticOutputConfig> sample_output_configs {};
    StaticSpan<StaticEventInputConfig> event_input_configs {};
    StaticSpan<StaticEventOutputConfig> event_output_configs {};

    NodeBundleHandle subgraph_boundary = 0;
    size_t subgraph_child_begin = 0;
    size_t subgraph_child_count = 0;
    StaticString subgraph_kind {};
    size_t subgraph_sample_input_count = 0;
    size_t subgraph_sample_output_count = 0;
    size_t subgraph_event_input_count = 0;
    size_t subgraph_event_output_count = 0;

    StaticSpan<size_t> virtual_node_handles {};
    StaticSpan<StaticSourceInfo> source_infos {};
};

struct StaticAuthoredSampleConnection {
    ChannelTypeId source_type = ChannelTypeId::mono;
    StaticSpan<SampleOutputChannelId> source_channels {};
    ChannelTypeId target_type = ChannelTypeId::mono;
    StaticSpan<SampleInputChannelId> target_channels {};
};

struct StaticAuthoredEventConnection {
    EventTypeId source_type = EventTypeId::empty;
    StaticSpan<EventOutputPortId> sources {};
    EventTypeId target_type = EventTypeId::empty;
    StaticSpan<EventInputPortId> targets {};
};

struct StaticAuthoredDetachedSamplePortInfo {
    size_t detach_id = 0;
    ChannelTypeId source_type = ChannelTypeId::mono;
    StaticSpan<SampleOutputChannelId> source_channels {};
    NodeBundleHandle writer_bundle = 0;
    NodeBundleHandle reader_bundle = 0;
    SampleOutputChannelId reader_channel {};
    size_t loop_extra_latency = 1;
};

template<class ChannelId>
struct StaticVirtualSamplePortMapping {
    StaticString name {};
    size_t ordinal = 0;
    ChannelLayout channel_layout {};
    StaticSpan<ChannelId> channels {};
    StaticSpan<StaticSpan<ChannelId>> member_channels {};
};

struct StaticVirtualEventPortMapping {
    StaticString name {};
    size_t ordinal = 0;
    EventTypeId type = EventTypeId::empty;
    StaticSpan<NodeBundlePortId> node_bundle_ports {};
};

struct StaticVirtualNodeRecord {
    StaticString id {};
    StaticString source_identity {};
    StaticString type_identity {};
    StaticSpan<StaticSourceInfo> source_infos {};
    StaticSpan<NodeBundleHandle> node_bundle_handles {};
    StaticSpan<StaticVirtualSamplePortMapping<SampleInputChannelId>>
        sample_inputs {};
    StaticSpan<StaticVirtualSamplePortMapping<SampleOutputChannelId>>
        sample_outputs {};
    StaticSpan<StaticVirtualEventPortMapping> event_inputs {};
    StaticSpan<StaticVirtualEventPortMapping> event_outputs {};
};

struct StaticPublicSamplePortMember {
    StaticString family_name {};
    ChannelTypeId channel_type = ChannelTypeId::mono;
    size_t channel_index = 0;
    bool whole_stream = false;
};

struct StaticAuthoredPublicPorts {
    NodeBundleHandle boundary = 0;
    StaticSpan<StaticSpan<StaticSourceInfo>> sample_input_source_infos {};
    StaticSpan<StaticSpan<StaticSourceInfo>> event_input_source_infos {};
    StaticSpan<StaticPublicSamplePortMember> sample_output_members {};
    StaticSpan<size_t> last_sample_output_port_ordinals {};
    StaticSpan<StaticSpan<StaticSourceInfo>> sample_output_source_infos {};
    StaticSpan<StaticSpan<StaticSourceInfo>> event_output_source_infos {};
    bool sample_outputs_defined = false;
};

struct AuthoredGraphView {
    StaticString identity {};
    StaticSpan<StaticAuthoredNodeBundle> node_bundles {};
    StaticSpan<StaticAuthoredSampleConnection> sample_connections {};
    StaticSpan<StaticAuthoredEventConnection> event_connections {};
    StaticAuthoredPublicPorts public_ports {};
    size_t next_detach_id = 0;
    StaticSpan<StaticAuthoredDetachedSamplePortInfo> detached_infos {};
    StaticSpan<StaticVirtualNodeRecord> virtual_nodes {};
};

namespace details {
consteval StaticSourceInfo freeze_source_info(SourceInfo const& info) {
    return {
        .declaration_identity = define_static_string(info.declaration_identity),
        .file_path = define_static_string(info.span.file_path),
        .begin = info.span.begin,
        .end = info.span.end,
    };
}

consteval StaticSpan<StaticSourceInfo> freeze_source_infos(
    std::span<SourceInfo const> infos) {
    std::vector<StaticSourceInfo> result;
    result.reserve(infos.size());
    for (auto const& info : infos) result.push_back(freeze_source_info(info));
    return define_static_span(result);
}

consteval StaticSpan<StaticSpan<StaticSourceInfo>> freeze_source_info_groups(
    std::span<std::vector<SourceInfo> const> groups) {
    std::vector<StaticSpan<StaticSourceInfo>> result;
    result.reserve(groups.size());
    for (auto const& group : groups) result.push_back(freeze_source_infos(group));
    return define_static_span(result);
}

consteval StaticNodePorts freeze_node_ports(NodePorts const& ports) {
    return {
        .sample_inputs = define_static_configs<StaticInputConfig>(
            ports.sample_inputs),
        .sample_outputs = define_static_configs<StaticOutputConfig>(
            ports.sample_outputs),
        .event_inputs = define_static_configs<StaticEventInputConfig>(
            ports.event_input_configs),
        .event_outputs = define_static_configs<StaticEventOutputConfig>(
            ports.event_output_configs),
    };
}

consteval StaticAuthoredNodeBundle freeze_node_bundle(
    AuthoredNodeBundleRecord const& record) {
    return {
        .kind = record.kind,
        .ports = freeze_node_ports(record.ports),
        .operations = record.operations,
        .has_ttl_samples = record.lifetime.ttl_samples.has_value(),
        .ttl_samples = record.lifetime.ttl_samples.value_or(0),
        .type_identity = define_static_string(record.type_identity),
        .reflected_type_name = define_static_string(record.reflected_type_name),
        .internal_latency_samples = record.internal_latency_samples,
        .maximum_block_size = record.maximum_block_size,
        .has_default_ttl_samples = record.default_ttl_samples.has_value(),
        .default_ttl_samples = record.default_ttl_samples.value_or(0),
        .block_skippable = record.block_skippable,
        .has_static_sample_value = record.static_sample_value.has_value(),
        .static_sample_value = record.static_sample_value.value_or(Sample{}),
        .has_deferred_detach = record.deferred_detach.has_value(),
        .deferred_detach = record.deferred_detach
            ? StaticDeferredDetachNode{
                  .kind = record.deferred_detach->kind,
                  .id = record.deferred_detach->id,
                  .loop_extra_latency =
                      record.deferred_detach->loop_extra_latency}
            : StaticDeferredDetachNode{},
        .tiled_members = define_static_span(record.tiled_members),
        .sample_input_configs = define_static_configs<StaticInputConfig>(
            record.sample_input_configs),
        .sample_output_configs = define_static_configs<StaticOutputConfig>(
            record.sample_output_configs),
        .event_input_configs = define_static_configs<StaticEventInputConfig>(
            record.event_input_configs),
        .event_output_configs = define_static_configs<StaticEventOutputConfig>(
            record.event_output_configs),
        .subgraph_boundary = record.subgraph_boundary,
        .subgraph_child_begin = record.subgraph_child_begin,
        .subgraph_child_count = record.subgraph_child_count,
        .subgraph_kind = define_static_string(record.subgraph_kind),
        .subgraph_sample_input_count = record.subgraph_sample_input_count,
        .subgraph_sample_output_count = record.subgraph_sample_output_count,
        .subgraph_event_input_count = record.subgraph_event_input_count,
        .subgraph_event_output_count = record.subgraph_event_output_count,
        .virtual_node_handles = define_static_span(record.virtual_node_handles),
        .source_infos = freeze_source_infos(record.source_infos),
    };
}

template<class ChannelId>
consteval StaticVirtualSamplePortMapping<ChannelId>
freeze_virtual_sample_port_mapping(
    VirtualSamplePortMapping<ChannelId> const& mapping) {
    std::vector<StaticSpan<ChannelId>> members;
    members.reserve(mapping.member_channels.size());
    for (auto const& member : mapping.member_channels)
        members.push_back(define_static_span(member));
    return {
        .name = define_static_string(mapping.name),
        .ordinal = mapping.ordinal,
        .channel_layout = mapping.channel_layout,
        .channels = define_static_span(mapping.channels),
        .member_channels = define_static_span(members),
    };
}

consteval StaticVirtualEventPortMapping freeze_virtual_event_port_mapping(
    VirtualEventPortMapping const& mapping) {
    return {
        .name = define_static_string(mapping.name),
        .ordinal = mapping.ordinal,
        .type = mapping.type,
        .node_bundle_ports = define_static_span(mapping.node_bundle_ports),
    };
}

consteval StaticVirtualNodeRecord freeze_virtual_node_record(
    VirtualNodeRecord const& record) {
    std::vector<StaticVirtualSamplePortMapping<SampleInputChannelId>>
        sample_inputs;
    std::vector<StaticVirtualSamplePortMapping<SampleOutputChannelId>>
        sample_outputs;
    std::vector<StaticVirtualEventPortMapping> event_inputs;
    std::vector<StaticVirtualEventPortMapping> event_outputs;
    for (auto const& mapping : record.sample_inputs)
        sample_inputs.push_back(freeze_virtual_sample_port_mapping(mapping));
    for (auto const& mapping : record.sample_outputs)
        sample_outputs.push_back(freeze_virtual_sample_port_mapping(mapping));
    for (auto const& mapping : record.event_inputs)
        event_inputs.push_back(freeze_virtual_event_port_mapping(mapping));
    for (auto const& mapping : record.event_outputs)
        event_outputs.push_back(freeze_virtual_event_port_mapping(mapping));
    return {
        .id = define_static_string(record.id),
        .source_identity = define_static_string(record.source_identity),
        .type_identity = define_static_string(record.type_identity),
        .source_infos = freeze_source_infos(record.source_infos),
        .node_bundle_handles = define_static_span(record.node_bundle_handles),
        .sample_inputs = define_static_span(sample_inputs),
        .sample_outputs = define_static_span(sample_outputs),
        .event_inputs = define_static_span(event_inputs),
        .event_outputs = define_static_span(event_outputs),
    };
}
} // namespace details

consteval AuthoredGraphView freeze_authored_graph(AuthoredGraph const& authored) {
    auto const records = authored.node_bundles.authored_records();
    std::vector<StaticAuthoredNodeBundle> bundles;
    bundles.reserve(records.size());
    for (auto const& record : records)
        bundles.push_back(details::freeze_node_bundle(record));

    std::vector<StaticAuthoredSampleConnection> sample_connections;
    for (auto const& connection : authored.connections.authored_sample_connections()) {
        sample_connections.push_back({
            .source_type = connection.source_type,
            .source_channels = details::define_static_span(
                connection.source_channels),
            .target_type = connection.target_type,
            .target_channels = details::define_static_span(
                connection.target_channels),
        });
    }
    std::vector<StaticAuthoredEventConnection> event_connections;
    for (auto const& connection : authored.connections.authored_event_connections()) {
        event_connections.push_back({
            .source_type = connection.source_type,
            .sources = details::define_static_span(connection.sources),
            .target_type = connection.target_type,
            .targets = details::define_static_span(connection.targets),
        });
    }

    auto const public_ports = authored.public_ports.authored_record();
    std::vector<StaticPublicSamplePortMember> public_members;
    for (auto const& member : public_ports.sample_output_members) {
        public_members.push_back({
            .family_name = details::define_static_string(member.family_name),
            .channel_type = member.channel_type,
            .channel_index = member.channel_index,
            .whole_stream = member.whole_stream,
        });
    }

    std::vector<StaticAuthoredDetachedSamplePortInfo> detached_infos;
    for (auto const& info : authored.detach.authored_infos()) {
        detached_infos.push_back({
            .detach_id = info.detach_id,
            .source_type = info.source_type,
            .source_channels = details::define_static_span(info.source_channels),
            .writer_bundle = info.writer_bundle,
            .reader_bundle = info.reader_bundle,
            .reader_channel = info.reader_channel,
            .loop_extra_latency = info.loop_extra_latency,
        });
    }

    std::vector<StaticVirtualNodeRecord> virtual_nodes;
    for (auto const& record : authored.virtual_nodes.records())
        virtual_nodes.push_back(details::freeze_virtual_node_record(record));

    return {
        .identity = details::define_static_string(authored.identity.value),
        .node_bundles = details::define_static_span(bundles),
        .sample_connections = details::define_static_span(sample_connections),
        .event_connections = details::define_static_span(event_connections),
        .public_ports = {
            .boundary = public_ports.boundary,
            .sample_input_source_infos =
                details::freeze_source_info_groups(
                    public_ports.sample_input_source_infos),
            .event_input_source_infos =
                details::freeze_source_info_groups(
                    public_ports.event_input_source_infos),
            .sample_output_members = details::define_static_span(public_members),
            .last_sample_output_port_ordinals = details::define_static_span(
                public_ports.last_sample_output_port_ordinals),
            .sample_output_source_infos =
                details::freeze_source_info_groups(
                    public_ports.sample_output_source_infos),
            .event_output_source_infos =
                details::freeze_source_info_groups(
                    public_ports.event_output_source_infos),
            .sample_outputs_defined = public_ports.sample_outputs_defined,
        },
        .next_detach_id = authored.detach.next_detach_id(),
        .detached_infos = details::define_static_span(detached_infos),
        .virtual_nodes = details::define_static_span(virtual_nodes),
    };
}

namespace details {
inline SourceInfo thaw_source_info(StaticSourceInfo info) {
    return {
        .declaration_identity = std::string(info.declaration_identity.view()),
        .span = {
            .file_path = std::string(info.file_path.view()),
            .begin = info.begin,
            .end = info.end,
        },
    };
}

inline std::vector<SourceInfo> thaw_source_infos(
    StaticSpan<StaticSourceInfo> infos) {
    std::vector<SourceInfo> result;
    result.reserve(infos.size);
    for (auto const& info : infos) result.push_back(thaw_source_info(info));
    return result;
}

inline std::vector<std::vector<SourceInfo>> thaw_source_info_groups(
    StaticSpan<StaticSpan<StaticSourceInfo>> groups) {
    std::vector<std::vector<SourceInfo>> result;
    result.reserve(groups.size);
    for (auto const& group : groups) result.push_back(thaw_source_infos(group));
    return result;
}

template<class StaticConfig>
inline auto thaw_configs(StaticSpan<StaticConfig> configs) {
    using Config = decltype(configs.data[0].config());
    std::vector<Config> result;
    result.reserve(configs.size);
    for (auto const& config : configs) result.push_back(config.config());
    return result;
}

inline NodePorts thaw_node_ports(StaticNodePorts ports) {
    return {
        .sample_inputs = thaw_configs(ports.sample_inputs),
        .sample_outputs = thaw_configs(ports.sample_outputs),
        .event_input_configs = thaw_configs(ports.event_inputs),
        .event_output_configs = thaw_configs(ports.event_outputs),
    };
}

inline AuthoredNodeBundleRecord thaw_node_bundle(
    StaticAuthoredNodeBundle record) {
    AuthoredNodeBundleRecord result;
    result.kind = record.kind;
    result.ports = thaw_node_ports(record.ports);
    result.operations = record.operations;
    if (record.has_ttl_samples) result.lifetime.ttl_samples = record.ttl_samples;
    result.type_identity = std::string(record.type_identity.view());
    result.reflected_type_name = std::string(record.reflected_type_name.view());
    result.internal_latency_samples = record.internal_latency_samples;
    result.maximum_block_size = record.maximum_block_size;
    if (record.has_default_ttl_samples)
        result.default_ttl_samples = record.default_ttl_samples;
    result.block_skippable = record.block_skippable;
    if (record.has_static_sample_value)
        result.static_sample_value = record.static_sample_value;
    if (record.has_deferred_detach) {
        result.deferred_detach = DeferredDetachNode{
            .kind = record.deferred_detach.kind,
            .id = record.deferred_detach.id,
            .loop_extra_latency = record.deferred_detach.loop_extra_latency,
        };
    }
    result.tiled_members.assign(
        record.tiled_members.begin(), record.tiled_members.end());
    result.sample_input_configs = thaw_configs(record.sample_input_configs);
    result.sample_output_configs = thaw_configs(record.sample_output_configs);
    result.event_input_configs = thaw_configs(record.event_input_configs);
    result.event_output_configs = thaw_configs(record.event_output_configs);
    result.subgraph_boundary = record.subgraph_boundary;
    result.subgraph_child_begin = record.subgraph_child_begin;
    result.subgraph_child_count = record.subgraph_child_count;
    result.subgraph_kind = std::string(record.subgraph_kind.view());
    result.subgraph_sample_input_count = record.subgraph_sample_input_count;
    result.subgraph_sample_output_count = record.subgraph_sample_output_count;
    result.subgraph_event_input_count = record.subgraph_event_input_count;
    result.subgraph_event_output_count = record.subgraph_event_output_count;
    result.virtual_node_handles.assign(
        record.virtual_node_handles.begin(), record.virtual_node_handles.end());
    result.source_infos = thaw_source_infos(record.source_infos);
    return result;
}

template<class ChannelId>
inline VirtualSamplePortMapping<ChannelId> thaw_virtual_sample_port_mapping(
    StaticVirtualSamplePortMapping<ChannelId> mapping) {
    VirtualSamplePortMapping<ChannelId> result;
    result.name = std::string(mapping.name.view());
    result.ordinal = mapping.ordinal;
    result.channel_layout = mapping.channel_layout;
    result.channels.assign(mapping.channels.begin(), mapping.channels.end());
    result.member_channels.reserve(mapping.member_channels.size);
    for (auto const& member : mapping.member_channels)
        result.member_channels.emplace_back(member.begin(), member.end());
    return result;
}

inline VirtualEventPortMapping thaw_virtual_event_port_mapping(
    StaticVirtualEventPortMapping mapping) {
    return {
        .name = std::string(mapping.name.view()),
        .ordinal = mapping.ordinal,
        .type = mapping.type,
        .node_bundle_ports = {
            mapping.node_bundle_ports.begin(),
            mapping.node_bundle_ports.end()},
    };
}

inline VirtualNodeRecord thaw_virtual_node_record(
    StaticVirtualNodeRecord record) {
    VirtualNodeRecord result;
    result.id = std::string(record.id.view());
    result.source_identity = std::string(record.source_identity.view());
    result.type_identity = std::string(record.type_identity.view());
    result.source_infos = thaw_source_infos(record.source_infos);
    result.node_bundle_handles.assign(
        record.node_bundle_handles.begin(), record.node_bundle_handles.end());
    result.sample_inputs.reserve(record.sample_inputs.size);
    for (auto const& mapping : record.sample_inputs)
        result.sample_inputs.push_back(thaw_virtual_sample_port_mapping(mapping));
    result.sample_outputs.reserve(record.sample_outputs.size);
    for (auto const& mapping : record.sample_outputs)
        result.sample_outputs.push_back(thaw_virtual_sample_port_mapping(mapping));
    result.event_inputs.reserve(record.event_inputs.size);
    for (auto const& mapping : record.event_inputs)
        result.event_inputs.push_back(thaw_virtual_event_port_mapping(mapping));
    result.event_outputs.reserve(record.event_outputs.size);
    for (auto const& mapping : record.event_outputs)
        result.event_outputs.push_back(thaw_virtual_event_port_mapping(mapping));
    return result;
}
} // namespace details

inline AuthoredGraph thaw_authored_graph(AuthoredGraphView view) {
    std::vector<AuthoredNodeBundleRecord> bundle_records;
    bundle_records.reserve(view.node_bundles.size);
    for (auto const& record : view.node_bundles)
        bundle_records.push_back(details::thaw_node_bundle(record));

    std::vector<AuthoredSampleConnection> sample_connections;
    sample_connections.reserve(view.sample_connections.size);
    for (auto const& connection : view.sample_connections) {
        sample_connections.push_back({
            .source_type = connection.source_type,
            .source_channels = {
                connection.source_channels.begin(),
                connection.source_channels.end()},
            .target_type = connection.target_type,
            .target_channels = {
                connection.target_channels.begin(),
                connection.target_channels.end()},
        });
    }
    std::vector<AuthoredEventConnection> event_connections;
    event_connections.reserve(view.event_connections.size);
    for (auto const& connection : view.event_connections) {
        event_connections.push_back({
            .source_type = connection.source_type,
            .sources = {connection.sources.begin(), connection.sources.end()},
            .target_type = connection.target_type,
            .targets = {connection.targets.begin(), connection.targets.end()},
        });
    }

    AuthoredPublicPortsRecord public_ports;
    public_ports.boundary = view.public_ports.boundary;
    public_ports.sample_input_source_infos = details::thaw_source_info_groups(
        view.public_ports.sample_input_source_infos);
    public_ports.event_input_source_infos = details::thaw_source_info_groups(
        view.public_ports.event_input_source_infos);
    for (auto const& member : view.public_ports.sample_output_members) {
        public_ports.sample_output_members.push_back({
            .family_name = std::string(member.family_name.view()),
            .channel_type = member.channel_type,
            .channel_index = member.channel_index,
            .whole_stream = member.whole_stream,
        });
    }
    public_ports.last_sample_output_port_ordinals.assign(
        view.public_ports.last_sample_output_port_ordinals.begin(),
        view.public_ports.last_sample_output_port_ordinals.end());
    public_ports.sample_output_source_infos = details::thaw_source_info_groups(
        view.public_ports.sample_output_source_infos);
    public_ports.event_output_source_infos = details::thaw_source_info_groups(
        view.public_ports.event_output_source_infos);
    public_ports.sample_outputs_defined = view.public_ports.sample_outputs_defined;

    std::vector<AuthoredDetachedSamplePortInfo> detached_infos;
    detached_infos.reserve(view.detached_infos.size);
    for (auto const& info : view.detached_infos) {
        detached_infos.push_back({
            .detach_id = info.detach_id,
            .source_type = info.source_type,
            .source_channels = {
                info.source_channels.begin(), info.source_channels.end()},
            .writer_bundle = info.writer_bundle,
            .reader_bundle = info.reader_bundle,
            .reader_channel = info.reader_channel,
            .loop_extra_latency = info.loop_extra_latency,
        });
    }

    std::vector<VirtualNodeRecord> virtual_records;
    virtual_records.reserve(view.virtual_nodes.size);
    for (auto const& record : view.virtual_nodes)
        virtual_records.push_back(details::thaw_virtual_node_record(record));

    return {
        .identity = GraphBuilderIdentity{std::string(view.identity.view())},
        .node_bundles = GraphBuilderNodeBundles::from_authored_records(
            bundle_records),
        .connections = GraphBuilderConnections::from_authored_connections(
            sample_connections, event_connections),
        .public_ports = GraphBuilderPublicPorts::from_authored_record(
            public_ports),
        .detach = GraphBuilderDetach::from_authored_infos(
            view.next_detach_id, detached_infos),
        .annotations = {},
        .virtual_nodes = GraphBuilderVirtualNodes::from_authored_records(
            virtual_records),
    };
}
} // namespace iv
