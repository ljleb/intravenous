#pragma once

#include <intravenous/graph/authored_graph.hpp>
#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/module/abi.h>
#include <intravenous/node/node_state_structure.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv::binary_wire_details {

inline constexpr std::uint32_t archive_magic = 0x49564147; // IVAG
inline constexpr std::uint32_t archive_version = 1;

class Writer {
public:
    template<class T>
    void pod(T value) requires std::is_trivially_copyable_v<T>
    {
        auto bytes = std::as_bytes(std::span{&value, std::size_t{1}});
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    void size(std::size_t value)
    {
        static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
        pod(static_cast<std::uint64_t>(value));
    }

    void flag(bool value) { pod(static_cast<std::uint8_t>(value)); }

    void string(std::string_view value)
    {
        size(value.size());
        auto bytes = std::as_bytes(std::span{value.data(), value.size()});
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    void append(std::span<std::byte const> value)
    {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    template<class Range, class Fn>
    void list(Range const& values, Fn&& write)
    {
        size(values.size());
        for (auto const& value : values) write(value);
    }

    std::vector<std::byte> take() && { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_{};
};

class Reader {
public:
    explicit Reader(std::span<std::byte const> bytes) : bytes_(bytes) {}

    template<class T>
    T pod() requires std::is_trivially_copyable_v<T>
    {
        require(sizeof(T), "truncated scalar");
        T result;
        std::memcpy(&result, bytes_.data() + cursor_, sizeof(T));
        cursor_ += sizeof(T);
        return result;
    }

    std::size_t size()
    {
        auto const value = pod<std::uint64_t>();
        if (value > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("authored graph archive size does not fit this process");
        return static_cast<std::size_t>(value);
    }

    bool flag()
    {
        auto const value = pod<std::uint8_t>();
        if (value > 1) throw std::runtime_error("authored graph archive has invalid boolean");
        return value != 0;
    }

    std::string string()
    {
        auto const length = size();
        require(length, "truncated string");
        auto const* first = reinterpret_cast<char const*>(bytes_.data() + cursor_);
        cursor_ += length;
        return {first, length};
    }

    std::size_t count()
    {
        auto const value = size();
        if (value > remaining())
            throw std::runtime_error("authored graph archive count exceeds remaining bytes");
        return value;
    }

    void finish() const
    {
        if (cursor_ != bytes_.size())
            throw std::runtime_error("authored graph archive has trailing bytes");
    }

private:
    std::size_t remaining() const { return bytes_.size() - cursor_; }

    void require(std::size_t length, char const* context)
    {
        if (length > remaining())
            throw std::runtime_error(std::string("authored graph archive: ") + context);
    }

    std::span<std::byte const> bytes_{};
    std::size_t cursor_ = 0;
};

template<class E> void write_enum(Writer& w, E value)
{
    w.pod(static_cast<std::underlying_type_t<E>>(value));
}

template<class E> E read_enum(Reader& r)
{
    return static_cast<E>(r.pod<std::underlying_type_t<E>>());
}

template<class T, class Fn> std::vector<T> read_list(Reader& r, Fn&& read)
{
    auto const count = r.count();
    std::vector<T> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) result.push_back(read());
    return result;
}

inline void write_source_info(Writer& w, SourceInfo const& value)
{
    w.string(value.declaration_identity);
    w.string(value.span.file_path);
    w.pod(value.span.begin);
    w.pod(value.span.end);
}

inline SourceInfo read_source_info(Reader& r)
{
    return {.declaration_identity = r.string(), .span = {
        .file_path = r.string(), .begin = r.pod<std::uint32_t>(), .end = r.pod<std::uint32_t>()}};
}

template<class Range> void write_source_infos(Writer& w, Range const& values)
{
    w.list(values, [&](SourceInfo const& value) { write_source_info(w, value); });
}

inline std::vector<SourceInfo> read_source_infos(Reader& r)
{
    return read_list<SourceInfo>(r, [&] { return read_source_info(r); });
}

inline void write_source_info_groups(Writer& w, std::vector<std::vector<SourceInfo>> const& values)
{
    w.list(values, [&](auto const& value) { write_source_infos(w, value); });
}

inline std::vector<std::vector<SourceInfo>> read_source_info_groups(Reader& r)
{
    return read_list<std::vector<SourceInfo>>(r, [&] { return read_source_infos(r); });
}

inline void write_layout(Writer& w, ChannelLayout value)
{
    write_enum(w, value.channel_type);
    write_enum(w, value.sample_layout);
}

inline ChannelLayout read_layout(Reader& r)
{
    return {.channel_type = read_enum<ChannelTypeId>(r), .sample_layout = read_enum<SampleStreamLayout>(r)};
}

inline void write_input(Writer& w, InputConfig const& value)
{
    w.string(value.name); write_layout(w, value.channel_layout); w.size(value.history);
    w.pod(value.default_value.value); w.pod(value.min.value); w.pod(value.max.value);
}

inline InputConfig read_input(Reader& r)
{
    return {.name = r.string(), .channel_layout = read_layout(r), .history = r.size(),
        .default_value = Sample{r.pod<Sample::storage>()}, .min = Sample{r.pod<Sample::storage>()},
        .max = Sample{r.pod<Sample::storage>()}};
}

inline void write_output(Writer& w, OutputConfig const& value)
{
    w.string(value.name); write_layout(w, value.channel_layout); w.size(value.latency); w.size(value.history);
}

inline OutputConfig read_output(Reader& r)
{
    return {.name = r.string(), .channel_layout = read_layout(r), .latency = r.size(), .history = r.size()};
}

inline void write_event_input(Writer& w, EventInputConfig const& value)
{
    w.string(value.name); write_enum(w, value.type);
}

inline EventInputConfig read_event_input(Reader& r)
{
    return {.name = r.string(), .type = read_enum<EventTypeId>(r)};
}

inline void write_event_output(Writer& w, EventOutputConfig const& value)
{
    w.string(value.name); write_enum(w, value.type);
}

inline EventOutputConfig read_event_output(Reader& r)
{
    return {.name = r.string(), .type = read_enum<EventTypeId>(r)};
}

template<class T, class Fn> void write_configs(Writer& w, std::span<T const> values, Fn&& write)
{
    w.list(values, [&](T const& value) { write(w, value); });
}

template<class T, class Fn> std::vector<T> read_configs(Reader& r, Fn&& read)
{
    return read_list<T>(r, [&] { return read(r); });
}

inline void write_ports(Writer& w, NodePorts const& value)
{
    write_configs<InputConfig>(w, value.sample_inputs, write_input);
    write_configs<OutputConfig>(w, value.sample_outputs, write_output);
    write_configs<EventInputConfig>(w, value.event_input_configs, write_event_input);
    write_configs<EventOutputConfig>(w, value.event_output_configs, write_event_output);
}

inline NodePorts read_ports(Reader& r)
{
    return {.sample_inputs = read_configs<InputConfig>(r, read_input),
        .sample_outputs = read_configs<OutputConfig>(r, read_output),
        .event_input_configs = read_configs<EventInputConfig>(r, read_event_input),
        .event_output_configs = read_configs<EventOutputConfig>(r, read_event_output)};
}

inline void write_code_key(Writer& w, NodeCodeKey value) { w.pod(value.low); w.pod(value.high); }
inline NodeCodeKey read_code_key(Reader& r) { return {.low = r.pod<std::uint64_t>(), .high = r.pod<std::uint64_t>()}; }

inline void write_bundle_port(Writer& w, NodeBundlePortId value)
{
    w.size(value.node_bundle_handle); write_enum(w, value.port_kind); w.size(value.port_ordinal);
}
inline NodeBundlePortId read_bundle_port(Reader& r)
{
    return {.node_bundle_handle = r.size(), .port_kind = read_enum<PortKind>(r), .port_ordinal = r.size()};
}

inline void write_output_channel(Writer& w, SampleOutputChannelId value)
{
    w.size(value.bundle); w.size(value.port); w.size(value.channel);
}
inline SampleOutputChannelId read_output_channel(Reader& r)
{
    return {.bundle = r.size(), .port = r.size(), .channel = r.size()};
}
inline void write_input_channel(Writer& w, SampleInputChannelId value)
{
    w.size(value.bundle); w.size(value.port); w.size(value.channel);
}
inline SampleInputChannelId read_input_channel(Reader& r)
{
    return {.bundle = r.size(), .port = r.size(), .channel = r.size()};
}
inline void write_event_output_port(Writer& w, EventOutputPortId value) { w.size(value.bundle); w.size(value.port); }
inline EventOutputPortId read_event_output_port(Reader& r) { return {.bundle = r.size(), .port = r.size()}; }
inline void write_event_input_port(Writer& w, EventInputPortId value) { w.size(value.bundle); w.size(value.port); }
inline EventInputPortId read_event_input_port(Reader& r) { return {.bundle = r.size(), .port = r.size()}; }

template<class T, class Fn> void write_values(Writer& w, std::span<T const> values, Fn&& write)
{
    w.list(values, [&](T value) { write(w, value); });
}
template<class T, class Fn> std::vector<T> read_values(Reader& r, Fn&& read)
{
    return read_list<T>(r, [&] { return read(r); });
}

inline void write_state(Writer& w, NodeStateStructure const& value)
{
    w.size(value.size_bits); w.size(value.alignment_bits);
    w.list(value.fields, [&](NodeStateFieldStructure const& field) {
        w.string(field.name); w.string(field.type_name); w.size(field.bit_offset); w.size(field.size_bits);
        w.size(field.alignment_bits); w.flag(field.bit_width.has_value());
        if (field.bit_width) w.size(*field.bit_width);
    });
}

inline NodeStateStructure read_state(Reader& r)
{
    NodeStateStructure result{.size_bits = r.size(), .alignment_bits = r.size()};
    result.fields = read_list<NodeStateFieldStructure>(r, [&] {
        NodeStateFieldStructure field{.name = r.string(), .type_name = r.string(), .bit_offset = r.size(),
            .size_bits = r.size(), .alignment_bits = r.size()};
        if (r.flag()) field.bit_width = r.size();
        return field;
    });
    return result;
}

template<class Channel, class Fn> void write_virtual_sample(Writer& w, VirtualSamplePortMapping<Channel> const& value, Fn&& write_channel)
{
    w.string(value.name); w.size(value.ordinal); write_layout(w, value.channel_layout);
    write_values<Channel>(w, value.channels, write_channel);
    w.list(value.member_channels, [&](auto const& members) { write_values<Channel>(w, members, write_channel); });
}

template<class Channel, class Fn> VirtualSamplePortMapping<Channel> read_virtual_sample(Reader& r, Fn&& read_channel)
{
    VirtualSamplePortMapping<Channel> result{.name = r.string(), .ordinal = r.size(), .channel_layout = read_layout(r),
        .channels = read_values<Channel>(r, read_channel)};
    result.member_channels = read_list<std::vector<Channel>>(r, [&] { return read_values<Channel>(r, read_channel); });
    return result;
}

inline void write_virtual_event(Writer& w, VirtualEventPortMapping const& value)
{
    w.string(value.name); w.size(value.ordinal); write_enum(w, value.type);
    write_values<NodeBundlePortId>(w, value.node_bundle_ports, write_bundle_port);
}
inline VirtualEventPortMapping read_virtual_event(Reader& r)
{
    return {.name = r.string(), .ordinal = r.size(), .type = read_enum<EventTypeId>(r),
        .node_bundle_ports = read_values<NodeBundlePortId>(r, read_bundle_port)};
}

inline void write_virtual_node(Writer& w, VirtualNodeRecord const& value)
{
    w.string(value.id); w.string(value.source_identity); w.string(value.type_identity); write_source_infos(w, value.source_infos);
    w.list(value.node_bundle_handles, [&](std::size_t value) { w.size(value); });
    w.list(value.sample_inputs, [&](auto const& value) { write_virtual_sample(w, value, write_input_channel); });
    w.list(value.sample_outputs, [&](auto const& value) { write_virtual_sample(w, value, write_output_channel); });
    w.list(value.event_inputs, [&](auto const& value) { write_virtual_event(w, value); });
    w.list(value.event_outputs, [&](auto const& value) { write_virtual_event(w, value); });
}

inline VirtualNodeRecord read_virtual_node(Reader& r)
{
    VirtualNodeRecord result{.id = r.string(), .source_identity = r.string(), .type_identity = r.string(),
        .source_infos = read_source_infos(r), .node_bundle_handles = read_list<NodeBundleHandle>(r, [&] { return r.size(); })};
    result.sample_inputs = read_list<VirtualSampleInputPortMapping>(r, [&] { return read_virtual_sample<SampleInputChannelId>(r, read_input_channel); });
    result.sample_outputs = read_list<VirtualSampleOutputPortMapping>(r, [&] { return read_virtual_sample<SampleOutputChannelId>(r, read_output_channel); });
    result.event_inputs = read_list<VirtualEventPortMapping>(r, [&] { return read_virtual_event(r); });
    result.event_outputs = read_list<VirtualEventPortMapping>(r, [&] { return read_virtual_event(r); });
    return result;
}

inline details::NodeCompilerRecord const& find_type(std::span<details::NodeCompilerRecord const> types, NodeCodeKey key)
{
    auto const found = std::find_if(types.begin(), types.end(), [&](auto const& value) { return value.code_key == key; });
    if (found == types.end()) throw std::runtime_error("authored graph references an unknown node code key");
    return *found;
}

} // namespace iv::binary_wire_details

namespace iv {

inline SerializedAuthoredGraph serialize_binary_authored_graph(
    AuthoredGraph const& authored,
    std::span<std::pair<NodeCodeKey, NodeStateStructure> const> state_structures)
{
    using namespace binary_wire_details;
    SerializedAuthoredGraph result;
    Writer bundles;
    std::size_t bundle_count = 0;
    std::size_t config_ordinal = 0;
    authored.node_bundles.for_each_authored_bundle([&](AuthoredNodeBundleView view) {
        ++bundle_count;
        write_enum(bundles, view.kind);
        bundles.list(view.virtual_node_handles, [&](std::size_t value) { bundles.size(value); });
        write_source_infos(bundles, view.source_infos);
        if (view.kind == AuthoredNodeBundleKind::concrete) {
            if (!view.node_storage || !*view.node_storage || !view.code_key || !view.ports)
                throw std::runtime_error("concrete authored node has incomplete compiler storage");
            write_ports(bundles, *view.ports);
            write_code_key(bundles, *view.code_key);
            bundles.size(config_ordinal++);
            bundles.size(view.node_size);
            bundles.size(view.node_alignment);
            auto const has_ttl = view.lifetime && view.lifetime->ttl_samples;
            bundles.flag(has_ttl);
            if (has_ttl) bundles.size(*view.lifetime->ttl_samples);
            bundles.string(view.type_identity ? *view.type_identity : std::string{});
            bundles.string(view.reflected_type_name ? *view.reflected_type_name : std::string{});
            bundles.size(view.internal_latency_samples);
            bundles.size(view.maximum_block_size);
            auto const has_default_ttl = view.default_ttl_samples && *view.default_ttl_samples;
            bundles.flag(has_default_ttl);
            if (has_default_ttl) bundles.size(**view.default_ttl_samples);
            bundles.flag(view.block_skippable);
            auto const has_static_value = view.static_sample_value && *view.static_sample_value;
            bundles.flag(has_static_value);
            if (has_static_value) bundles.pod((**view.static_sample_value).value);
            auto const has_deferred = view.deferred_detach && *view.deferred_detach;
            bundles.flag(has_deferred);
            if (has_deferred) {
                auto const& deferred = **view.deferred_detach;
                write_enum(bundles, deferred.kind);
                bundles.size(deferred.id);
                bundles.size(deferred.loop_extra_latency);
            }
            auto const state = std::find_if(state_structures.begin(), state_structures.end(), [&](auto const& item) {
                return item.first == *view.code_key;
            });
            bundles.flag(state != state_structures.end());
            if (state != state_structures.end()) write_state(bundles, state->second);

            AuthoredNodeConfigBytes config;
            config.alignment = view.node_alignment;
            config.bytes.resize(view.node_size);
            std::memcpy(config.bytes.data(), (*view.node_storage).get(), view.node_size);
            if (view.config_string_relocations) {
                config.string_relocations = *view.config_string_relocations;
            }
            for (auto const& relocation : config.string_relocations) {
                if (relocation.byte_offset > config.bytes.size()
                    || config.bytes.size() - relocation.byte_offset < sizeof(char const*)) {
                    throw std::runtime_error("node configuration string relocation is out of bounds");
                }
            }
            result.node_configs.push_back(std::move(config));
        } else if (view.kind == AuthoredNodeBundleKind::tiled) {
            bundles.list(view.tiled_members, [&](std::size_t value) { bundles.size(value); });
            bundles.string(view.type_identity ? *view.type_identity : std::string{});
            write_configs<InputConfig>(bundles, view.sample_input_configs, write_input);
            write_configs<OutputConfig>(bundles, view.sample_output_configs, write_output);
            write_configs<EventInputConfig>(bundles, view.event_input_configs, write_event_input);
            write_configs<EventOutputConfig>(bundles, view.event_output_configs, write_event_output);
        } else if (view.kind == AuthoredNodeBundleKind::boundary) {
            write_configs<InputConfig>(bundles, view.sample_input_configs, write_input);
            write_configs<OutputConfig>(bundles, view.sample_output_configs, write_output);
            write_configs<EventInputConfig>(bundles, view.event_input_configs, write_event_input);
            write_configs<EventOutputConfig>(bundles, view.event_output_configs, write_event_output);
        } else if (view.kind == AuthoredNodeBundleKind::subgraph) {
            bundles.size(view.subgraph_boundary);
            bundles.size(view.subgraph_child_begin);
            bundles.size(view.subgraph_child_count);
            bundles.string(view.subgraph_kind ? *view.subgraph_kind : std::string{});
            auto const has_ttl = view.lifetime && view.lifetime->ttl_samples;
            bundles.flag(has_ttl);
            if (has_ttl) bundles.size(*view.lifetime->ttl_samples);
            bundles.string(view.type_identity ? *view.type_identity : std::string{});
            bundles.size(view.subgraph_sample_input_count);
            bundles.size(view.subgraph_sample_output_count);
            bundles.size(view.subgraph_event_input_count);
            bundles.size(view.subgraph_event_output_count);
        } else {
            throw std::runtime_error("authored graph has invalid bundle kind");
        }
    });

    Writer writer;
    writer.pod(archive_magic);
    writer.pod(archive_version);
    writer.string(authored.identity.value);
    writer.size(bundle_count);
    auto bundle_bytes = std::move(bundles).take();
    writer.append(bundle_bytes);

    auto const samples = authored.connections.authored_sample_connections();
    writer.list(samples, [&](AuthoredSampleConnection const& value) {
        write_enum(writer, value.source_type);
        write_values<SampleOutputChannelId>(writer, value.source_channels, write_output_channel);
        write_enum(writer, value.target_type);
        write_values<SampleInputChannelId>(writer, value.target_channels, write_input_channel);
    });
    auto const events = authored.connections.authored_event_connections();
    writer.list(events, [&](AuthoredEventConnection const& value) {
        write_enum(writer, value.source_type);
        write_values<EventOutputPortId>(writer, value.sources, write_event_output_port);
        write_enum(writer, value.target_type);
        write_values<EventInputPortId>(writer, value.targets, write_event_input_port);
    });

    auto const public_ports = authored.public_ports.authored_record();
    writer.size(public_ports.boundary);
    write_source_info_groups(writer, public_ports.sample_input_source_infos);
    write_source_info_groups(writer, public_ports.event_input_source_infos);
    writer.list(public_ports.last_sample_output_port_ordinals, [&](std::size_t value) { writer.size(value); });
    write_source_info_groups(writer, public_ports.sample_output_source_infos);
    write_source_info_groups(writer, public_ports.event_output_source_infos);
    writer.flag(public_ports.sample_outputs_defined);
    writer.list(public_ports.sample_output_members, [&](PublicSamplePortMember const& value) {
        writer.string(value.family_name);
        write_enum(writer, value.channel_type);
        writer.size(value.channel_index);
        writer.flag(value.whole_stream);
    });

    writer.size(authored.detach.next_detach_id());
    auto const detached = authored.detach.authored_infos();
    writer.list(detached, [&](AuthoredDetachedSamplePortInfo const& value) {
        writer.size(value.detach_id);
        write_enum(writer, value.source_type);
        write_values<SampleOutputChannelId>(writer, value.source_channels, write_output_channel);
        writer.size(value.writer_bundle);
        writer.size(value.reader_bundle);
        write_output_channel(writer, value.reader_channel);
        writer.size(value.loop_extra_latency);
    });
    auto const virtual_nodes = authored.virtual_nodes.records();
    writer.list(virtual_nodes, [&](VirtualNodeRecord const& value) { write_virtual_node(writer, value); });
    result.bytes = std::move(writer).take();
    return result;
}

inline AuthoredGraph deserialize_binary_authored_graph(
    std::span<std::byte const> bytes,
    std::span<details::NodeCompilerRecord const> node_types,
    std::span<ModuleNodeConfigRecord const> node_configs,
    std::span<std::shared_ptr<void const> const> node_config_storage = {})
{
    using namespace binary_wire_details;
    if (!node_config_storage.empty() && node_config_storage.size() != node_configs.size())
        throw std::runtime_error("module node config storage count does not match config table");
    Reader reader(bytes);
    if (reader.pod<std::uint32_t>() != archive_magic)
        throw std::runtime_error("unsupported authored graph archive magic");
    if (reader.pod<std::uint32_t>() != archive_version)
        throw std::runtime_error("unsupported authored graph archive version");
    auto identity = reader.string();
    auto const bundle_count = reader.count();
    std::vector<AuthoredNodeBundleRecord> bundles;
    bundles.reserve(bundle_count);
    for (std::size_t i = 0; i < bundle_count; ++i) {
        AuthoredNodeBundleRecord record;
        record.kind = read_enum<AuthoredNodeBundleKind>(reader);
        record.virtual_node_handles = read_list<std::size_t>(reader, [&] { return reader.size(); });
        record.source_infos = read_source_infos(reader);
        if (record.kind == AuthoredNodeBundleKind::concrete) {
            record.ports = read_ports(reader);
            record.code_key = read_code_key(reader);
            auto const ordinal = reader.size();
            if (ordinal >= node_configs.size()) throw std::runtime_error("authored graph config ordinal is out of range");
            auto const& config = node_configs[ordinal];
            record.node_size = reader.size();
            record.node_alignment = reader.size();
            if (config.size != record.node_size || config.alignment < record.node_alignment)
                throw std::runtime_error("module node config layout does not match authored graph");
            record.node_storage = node_config_storage.empty()
                ? std::shared_ptr<void const>(config.data, [](void const*) {})
                : node_config_storage[ordinal];
            record.operations = {.runtime = find_type(node_types, record.code_key).runtime};
            record.operations.runtime.node_data = config.data;
            if (reader.flag()) record.lifetime.ttl_samples = reader.size();
            record.type_identity = reader.string();
            record.reflected_type_name = reader.string();
            record.internal_latency_samples = reader.size();
            record.maximum_block_size = reader.size();
            if (reader.flag()) record.default_ttl_samples = reader.size();
            record.block_skippable = reader.flag();
            if (reader.flag()) record.static_sample_value = Sample{reader.pod<Sample::storage>()};
            if (reader.flag()) record.deferred_detach = DeferredDetachNode{
                .kind = read_enum<DeferredDetachNodeKind>(reader), .id = reader.size(), .loop_extra_latency = reader.size()};
            if (reader.flag()) {
                record.state_structure_storage = std::make_shared<NodeStateStructure>(read_state(reader));
                record.operations.runtime.state_structure = record.state_structure_storage.get();
            }
        } else if (record.kind == AuthoredNodeBundleKind::tiled) {
            record.tiled_members = read_list<NodeBundleHandle>(reader, [&] { return reader.size(); });
            record.type_identity = reader.string();
            record.sample_input_configs = read_configs<InputConfig>(reader, read_input);
            record.sample_output_configs = read_configs<OutputConfig>(reader, read_output);
            record.event_input_configs = read_configs<EventInputConfig>(reader, read_event_input);
            record.event_output_configs = read_configs<EventOutputConfig>(reader, read_event_output);
        } else if (record.kind == AuthoredNodeBundleKind::boundary) {
            record.sample_input_configs = read_configs<InputConfig>(reader, read_input);
            record.sample_output_configs = read_configs<OutputConfig>(reader, read_output);
            record.event_input_configs = read_configs<EventInputConfig>(reader, read_event_input);
            record.event_output_configs = read_configs<EventOutputConfig>(reader, read_event_output);
        } else if (record.kind == AuthoredNodeBundleKind::subgraph) {
            record.subgraph_boundary = reader.size();
            record.subgraph_child_begin = reader.size();
            record.subgraph_child_count = reader.size();
            record.subgraph_kind = reader.string();
            if (reader.flag()) record.lifetime.ttl_samples = reader.size();
            record.type_identity = reader.string();
            record.subgraph_sample_input_count = reader.size();
            record.subgraph_sample_output_count = reader.size();
            record.subgraph_event_input_count = reader.size();
            record.subgraph_event_output_count = reader.size();
        } else throw std::runtime_error("authored graph archive has invalid bundle kind");
        bundles.push_back(std::move(record));
    }

    auto sample_connections = read_list<AuthoredSampleConnection>(reader, [&] {
        return AuthoredSampleConnection{.source_type = read_enum<ChannelTypeId>(reader),
            .source_channels = read_values<SampleOutputChannelId>(reader, read_output_channel),
            .target_type = read_enum<ChannelTypeId>(reader),
            .target_channels = read_values<SampleInputChannelId>(reader, read_input_channel)};
    });
    auto event_connections = read_list<AuthoredEventConnection>(reader, [&] {
        return AuthoredEventConnection{.source_type = read_enum<EventTypeId>(reader),
            .sources = read_values<EventOutputPortId>(reader, read_event_output_port),
            .target_type = read_enum<EventTypeId>(reader),
            .targets = read_values<EventInputPortId>(reader, read_event_input_port)};
    });
    AuthoredPublicPortsRecord public_ports{.boundary = reader.size(),
        .sample_input_source_infos = read_source_info_groups(reader),
        .event_input_source_infos = read_source_info_groups(reader),
        .last_sample_output_port_ordinals = read_list<std::size_t>(reader, [&] { return reader.size(); }),
        .sample_output_source_infos = read_source_info_groups(reader),
        .event_output_source_infos = read_source_info_groups(reader),
        .sample_outputs_defined = reader.flag()};
    public_ports.sample_output_members = read_list<PublicSamplePortMember>(reader, [&] {
        return PublicSamplePortMember{.family_name = reader.string(), .channel_type = read_enum<ChannelTypeId>(reader),
            .channel_index = reader.size(), .whole_stream = reader.flag()};
    });
    auto const next_detach_id = reader.size();
    auto detached = read_list<AuthoredDetachedSamplePortInfo>(reader, [&] {
        return AuthoredDetachedSamplePortInfo{.detach_id = reader.size(), .source_type = read_enum<ChannelTypeId>(reader),
            .source_channels = read_values<SampleOutputChannelId>(reader, read_output_channel), .writer_bundle = reader.size(),
            .reader_bundle = reader.size(), .reader_channel = read_output_channel(reader), .loop_extra_latency = reader.size()};
    });
    auto virtual_nodes = read_list<VirtualNodeRecord>(reader, [&] { return read_virtual_node(reader); });
    reader.finish();
    return {.identity = GraphBuilderIdentity{std::move(identity)},
        .node_bundles = GraphBuilderNodeBundles::from_authored_records(bundles),
        .connections = GraphBuilderConnections::from_authored_connections(sample_connections, event_connections),
        .public_ports = GraphBuilderPublicPorts::from_authored_record(public_ports),
        .detach = GraphBuilderDetach::from_authored_infos(next_detach_id, detached), .annotations = {},
        .virtual_nodes = GraphBuilderVirtualNodes::from_authored_records(virtual_nodes)};
}

} // namespace iv
