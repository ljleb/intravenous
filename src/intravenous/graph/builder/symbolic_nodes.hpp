#pragma once

#include <intravenous/graph/builder/node_bundles.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iv {
using SymbolicNodeHandle = size_t;

template<class ChannelId>
struct SymbolicSamplePortMapping {
  std::string name{};
  size_t index = 0;
  ChannelLayout channel_layout{};
  std::vector<SourceInfo> source_infos{};
  std::vector<ChannelId> channels{};
  std::vector<std::vector<ChannelId>> member_channels{};
  bool operator==(SymbolicSamplePortMapping const&) const = default;
};
using SymbolicSampleInputPortMapping = SymbolicSamplePortMapping<SampleInputChannelId>;
using SymbolicSampleOutputPortMapping = SymbolicSamplePortMapping<SampleOutputChannelId>;

struct SymbolicEventPortMapping {
  std::string name{};
  size_t index = 0;
  EventTypeId type = EventTypeId::empty;
  std::vector<SourceInfo> source_infos{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
  bool operator==(SymbolicEventPortMapping const&) const = default;
};

struct SymbolicNodeRecord {
  std::string id{};
  std::string source_identity {};
  std::string type_identity {};
  std::vector<SourceInfo> source_infos{};
  std::vector<NodeBundleHandle> node_bundle_handles{};
  std::vector<SymbolicSampleInputPortMapping> sample_inputs{};
  std::vector<SymbolicSampleOutputPortMapping> sample_outputs{};
  std::vector<SymbolicEventPortMapping> event_inputs{};
  std::vector<SymbolicEventPortMapping> event_outputs{};
  // Complete expressions retain channel order even when two channels share
  // the same concrete source.
  std::vector<std::vector<SampleOutputRoute>> sample_output_routes{};

  constexpr bool has_lane() const {
    return std::ranges::none_of(source_infos, &SourceInfo::no_lane);
  }
};

struct GraphBuilderSymbolicSampleInputPort {
  SymbolicPortId id{};
  ContinuousInputConfig config{};
  std::vector<SampleInputChannelId> channels{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderSymbolicSampleOutputPort {
  SymbolicPortId id{};
  ContinuousOutputConfig config{};
  std::vector<SampleOutputChannelId> channels{};
  std::vector<std::vector<SampleOutputChannelId>> member_channels{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderSymbolicEventInputPort {
  SymbolicPortId id{};
  EventInputConfig config{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderSymbolicEventOutputPort {
  SymbolicPortId id{};
  EventOutputConfig config{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderSymbolicPorts {
  std::vector<GraphBuilderSymbolicSampleInputPort> sample_inputs{};
  std::vector<GraphBuilderSymbolicSampleOutputPort> sample_outputs{};
  std::vector<GraphBuilderSymbolicEventInputPort> event_inputs{};
  std::vector<GraphBuilderSymbolicEventOutputPort> event_outputs{};
};

// Symbolic-node records are append-only except for member/source-info
// accumulation. Which record a live declaration joins is decided by the
// builder's scope namespaces, so these operations address records by handle.
class GraphBuilderSymbolicNodes {
public:
  constexpr SymbolicNodeHandle create(
      std::string_view source_identity, std::string_view type_identity);
  constexpr void attach_bundle_member(
      GraphBuilderNodeBundles&, SymbolicNodeHandle, NodeBundleHandle,
      SourceInfo const* source_info = nullptr);
  constexpr void attach_sample_output(
      SymbolicNodeHandle, ChannelTypeId, std::span<SampleOutputRoute const>,
      SourceInfo const&);
  constexpr void attach_event_output(
      SymbolicNodeHandle, EventTypeId, std::span<EventOutputPortId const>,
      SourceInfo const&);
  constexpr void annotate_input_source_info(
      GraphBuilderNodeBundles&, SymbolicNodeHandle, NodeBundleHandle,
      PortKind, std::string_view port_name, SourceInfo const&);
  // Imports every child record as a distinct record. `qualifier` is
  // appended to each imported introspection ID; it names the placement
  // relative to the enclosing semantic scope, never a session handle.
  constexpr std::vector<SymbolicNodeHandle> import_child(
      GraphBuilderSymbolicNodes const&,
      size_t node_bundle_offset, std::string_view qualifier);
  // Appends `qualifier` to the IDs of records [begin, size()).
  constexpr void qualify(size_t begin, std::string_view qualifier);

  constexpr size_t size() const { return _records.size(); }
  constexpr std::vector<SymbolicNodeRecord> const& records() const;
  static constexpr GraphBuilderSymbolicNodes from_configured_records(
      std::span<SymbolicNodeRecord const>);
  constexpr GraphBuilderSymbolicPorts ports(
      GraphBuilderNodeBundles const&) const;
  constexpr SymbolicNodeRecord const& record(SymbolicNodeHandle) const;
  constexpr std::vector<std::string> ids_for_bundle(NodeBundle const&) const;

private:
    std::vector<SymbolicNodeRecord> _records {};
};
} // namespace iv
namespace iv {
namespace {
template <class Config>
constexpr void append_symbolic_event_port_mapping(
    std::vector<SymbolicEventPortMapping>& mappings, Config const& config,
    size_t index, NodeBundlePortId bundle_port) {
  if (mappings.size() <= index) mappings.resize(index + 1);
  auto& mapping = mappings[index];
  if (mapping.node_bundle_ports.empty()) {
    mapping = {.name = config.name, .index = index, .type = config.type,
               .node_bundle_ports = {bundle_port}};
    return;
  }
  if (mapping.name != config.name || mapping.type != config.type)
    details::error("symbolic node members must expose matching event port configurations");
  if (!std::ranges::contains(mapping.node_bundle_ports, bundle_port))
    mapping.node_bundle_ports.push_back(bundle_port);
}

template<class Mapping, class Config, class Channels>
constexpr void append_symbolic_sample_port_mapping(
    std::vector<Mapping>& mappings, Config const& config, size_t index,
    ChannelLayout layout, Channels const& channels) {
  if (mappings.size() <= index) mappings.resize(index + 1);
  auto& mapping = mappings[index];
  if (mapping.channels.empty()) {
    mapping = {.name = config.name, .index = index,
               .channel_layout = layout,
               .channels = {channels.begin(), channels.end()},
               .member_channels = {{channels.begin(), channels.end()}}};
    return;
  }
  if (mapping.name != config.name || mapping.channel_layout != layout)
    details::error("symbolic node members must expose matching sample port configurations");
  for (auto const channel : channels)
    if (!std::ranges::contains(mapping.channels, channel))
      mapping.channels.push_back(channel);
  mapping.member_channels.emplace_back(channels.begin(), channels.end());
}

constexpr void append_bundle_mappings(
    SymbolicNodeRecord& symbolic_node,
    GraphBuilderNodeBundles const& bundles,
    NodeBundleHandle handle) {
  auto const& bundle = bundles.bundle(handle);
  for (size_t index = 0; index < bundle.sample_input_count(); ++index) {
    NodeBundlePortId const port{handle, PortKind::sample, index};
    auto const config = bundles.resolve_sample_input(port).config;
    append_symbolic_sample_port_mapping(symbolic_node.sample_inputs, config, index,
                                       config.channel_layout,
                                       bundles.sample_input_channels(port));
  }
  for (size_t index = 0; index < bundle.sample_output_count(); ++index) {
    NodeBundlePortId const port{handle, PortKind::sample, index};
    auto const config = bundles.resolve_sample_output(port).config;
    append_symbolic_sample_port_mapping(symbolic_node.sample_outputs, config, index,
                                       config.channel_layout,
                                       bundles.sample_output_channels(port));
  }
  for (size_t index = 0; index < bundle.event_input_count(); ++index) {
    NodeBundlePortId const port{handle, PortKind::event, index};
    append_symbolic_event_port_mapping(symbolic_node.event_inputs,
        bundles.resolve_event_input(port).config, index, port);
  }
  for (size_t index = 0; index < bundle.event_output_count(); ++index) {
    NodeBundlePortId const port{handle, PortKind::event, index};
    append_symbolic_event_port_mapping(symbolic_node.event_outputs,
        bundles.resolve_event_output(port).config, index, port);
  }
}
} // namespace

constexpr SymbolicNodeHandle GraphBuilderSymbolicNodes::create(
    std::string_view source_identity, std::string_view type_identity
)
{
    auto const handle = _records.size();
    _records.push_back({
        .id = details::typed_symbolic_node_id(source_identity, type_identity),
        .source_identity = std::string(source_identity),
        .type_identity = std::string(type_identity),
    });
    return handle;
}

constexpr void GraphBuilderSymbolicNodes::attach_bundle_member(
    GraphBuilderNodeBundles& bundles, SymbolicNodeHandle symbolic_handle,
    NodeBundleHandle bundle_handle, SourceInfo const* source_info) {
  auto& record = _records[symbolic_handle];
  if (!std::ranges::contains(record.node_bundle_handles, bundle_handle)) {
    record.node_bundle_handles.push_back(bundle_handle);
    append_bundle_mappings(record, bundles, bundle_handle);
  }
  auto& inverse = bundles.bundle(bundle_handle).symbolic_node_handles();
  if (!std::ranges::contains(inverse, symbolic_handle)) inverse.push_back(symbolic_handle);
  if (source_info && !std::ranges::contains(record.source_infos, *source_info))
    record.source_infos.push_back(*source_info);
}

constexpr void GraphBuilderSymbolicNodes::attach_sample_output(
    SymbolicNodeHandle handle, ChannelTypeId channel_type,
    std::span<SampleOutputRoute const> routes,
    SourceInfo const& source_info) {
  if (routes.empty()) return;
  auto& record = _records[handle];
  if (!std::ranges::contains(record.source_infos, source_info))
    record.source_infos.push_back(source_info);
  if (record.sample_outputs.empty()) {
    std::vector<SampleOutputChannelId> channels;
    channels.reserve(routes.size());
    for (auto const& route : routes) channels.push_back(route.channel);
    record.sample_outputs.push_back({
        .name = "value",
        .index = 0,
        .channel_layout = {
            .channel_type = channel_type,
            .sample_layout = SampleStreamLayout::planar,
        },
        .channels = channels,
        .member_channels = {std::move(channels)},
    });
  }
  if (std::ranges::none_of(record.sample_output_routes, [&](auto const& existing) {
        return std::ranges::equal(existing, routes);
      })) record.sample_output_routes.emplace_back(routes.begin(), routes.end());
  for (auto const& route : routes) {
    auto const channel = route.channel;
    if (!std::ranges::contains(record.sample_outputs.front().channels, channel))
      record.sample_outputs.front().channels.push_back(channel);
    if (!std::ranges::contains(record.node_bundle_handles, channel.bundle))
      record.node_bundle_handles.push_back(channel.bundle);
  }
}

constexpr void GraphBuilderSymbolicNodes::attach_event_output(
    SymbolicNodeHandle handle, EventTypeId type,
    std::span<EventOutputPortId const> sources,
    SourceInfo const& source_info) {
  if (sources.empty()) return;
  auto& record = _records[handle];
  if (!std::ranges::contains(record.source_infos, source_info))
    record.source_infos.push_back(source_info);
  if (record.event_outputs.empty()) {
    record.event_outputs.push_back({
        .name = "value",
        .index = 0,
        .type = type,
    });
  }
  for (auto source : sources) {
    NodeBundlePortId const port{source.bundle, PortKind::event, source.port};
    if (!std::ranges::contains(
            record.event_outputs.front().node_bundle_ports, port))
      record.event_outputs.front().node_bundle_ports.push_back(port);
    if (!std::ranges::contains(record.node_bundle_handles, source.bundle))
      record.node_bundle_handles.push_back(source.bundle);
  }
}

constexpr void GraphBuilderSymbolicNodes::annotate_input_source_info(
    GraphBuilderNodeBundles& bundles,
    SymbolicNodeHandle handle,
    NodeBundleHandle bundle_handle,
    PortKind port_kind,
    std::string_view port_name,
    SourceInfo const& source_info) {
  attach_bundle_member(bundles, handle, bundle_handle, nullptr);
  auto& record = _records[handle];

  if (port_kind == PortKind::sample) {
    auto const mapping = std::ranges::find_if(
        record.sample_inputs,
        [&](auto const& candidate) { return candidate.name == port_name; });
    if (mapping == record.sample_inputs.end())
      details::error(
          "cannot annotate unknown sample input '" + std::string(port_name)
          + "' on symbolic node");
    if (!std::ranges::contains(mapping->source_infos, source_info))
      mapping->source_infos.push_back(source_info);
    return;
  }

  auto const mapping = std::ranges::find_if(
      record.event_inputs,
      [&](auto const& candidate) { return candidate.name == port_name; });
  if (mapping == record.event_inputs.end())
    details::error(
        "cannot annotate unknown event input '" + std::string(port_name)
        + "' on symbolic node");
  if (!std::ranges::contains(mapping->source_infos, source_info))
    mapping->source_infos.push_back(source_info);
}

constexpr std::vector<SymbolicNodeHandle> GraphBuilderSymbolicNodes::import_child(
    GraphBuilderSymbolicNodes const& child,
    size_t bundle_offset, std::string_view qualifier) {
  std::vector<SymbolicNodeHandle> translation;
  translation.reserve(child._records.size());

  auto remap_node_bundle_port = [bundle_offset](NodeBundlePortId& port) {
    port.node_bundle_handle += bundle_offset;
  };
  auto remap_sample_channel = [bundle_offset](auto& channel) {
    channel.bundle += bundle_offset;
  };

  // Imported symbolic nodes deliberately remain distinct records. They belong
  // to the embedded child scope and must not be coalesced into the parent's
  // root symbolic namespace merely because source/type identities match.
  for (auto child_record : child._records) {
    auto const handle = _records.size();
    translation.push_back(handle);

    // Introspection IDs are graph-local. Qualify an imported record by its
    // placement so equal local identities in two embeddings remain separate
    // scoped symbolic nodes. The source/type identities remain unchanged for
    // structured path matching.
    child_record.id += qualifier;

    for (auto& bundle : child_record.node_bundle_handles) {
      bundle += bundle_offset;
    }
    for (auto& mapping : child_record.sample_inputs) {
      for (auto& channel : mapping.channels) remap_sample_channel(channel);
      for (auto& members : mapping.member_channels) {
        for (auto& channel : members) remap_sample_channel(channel);
      }
    }
    for (auto& mapping : child_record.sample_outputs) {
      for (auto& channel : mapping.channels) remap_sample_channel(channel);
      for (auto& members : mapping.member_channels) {
        for (auto& channel : members) remap_sample_channel(channel);
      }
    }
    for (auto& expression : child_record.sample_output_routes)
      for (auto& route : expression) remap_sample_channel(route.channel);
    for (auto& mapping : child_record.event_inputs) {
      for (auto& port : mapping.node_bundle_ports) remap_node_bundle_port(port);
    }
    for (auto& mapping : child_record.event_outputs) {
      for (auto& port : mapping.node_bundle_ports) remap_node_bundle_port(port);
    }

    _records.push_back(std::move(child_record));
  }
  return translation;
}

constexpr void GraphBuilderSymbolicNodes::qualify(
    size_t begin, std::string_view qualifier) {
  if (qualifier.empty()) return;
  for (auto index = begin; index < _records.size(); ++index) {
    _records[index].id += qualifier;
  }
}

constexpr std::vector<SymbolicNodeRecord> const&
GraphBuilderSymbolicNodes::records() const {
  return _records;
}
constexpr GraphBuilderSymbolicNodes
GraphBuilderSymbolicNodes::from_configured_records(
    std::span<SymbolicNodeRecord const> records) {
  GraphBuilderSymbolicNodes result;
  result._records.assign(records.begin(), records.end());
  return result;
}
constexpr SymbolicNodeRecord const& GraphBuilderSymbolicNodes::record(
    SymbolicNodeHandle handle) const {
  return _records.at(handle);
}

constexpr GraphBuilderSymbolicPorts GraphBuilderSymbolicNodes::ports(
    GraphBuilderNodeBundles const& bundles) const {
  GraphBuilderSymbolicPorts result;
  auto sample_bundle_ports = [](auto const& channels) {
    std::vector<NodeBundlePortId> ports;
    for (auto const channel : channels) {
      NodeBundlePortId const port{channel.bundle, PortKind::sample, channel.port};
      if (!std::ranges::contains(ports, port)) ports.push_back(port);
    }
    return ports;
  };
  for (auto const& node : _records) {
    for (auto const& mapping : node.sample_inputs) {
      if (mapping.channels.empty()) continue;
      auto const first = mapping.channels.front();
      auto config = bundles.resolve_sample_input(
          {first.bundle, PortKind::sample, first.port}).config;
      set_continuous_channel_layout(config, mapping.channel_layout);
      result.sample_inputs.push_back({
          .id = {node.id, PortKind::sample, mapping.index},
          .config = std::move(config), .channels = mapping.channels,
          .node_bundle_ports = sample_bundle_ports(mapping.channels)});
    }
    for (auto const& mapping : node.sample_outputs) {
      if (mapping.channels.empty()) continue;
      auto const first = mapping.channels.front();
      auto config = bundles.resolve_sample_output(
          {first.bundle, PortKind::sample, first.port}).config;
      set_continuous_channel_layout(config, mapping.channel_layout);
      result.sample_outputs.push_back({
          .id = {node.id, PortKind::sample, mapping.index},
          .config = std::move(config), .channels = mapping.channels,
          .member_channels = mapping.member_channels,
          .node_bundle_ports = sample_bundle_ports(mapping.channels)});
    }
    for (auto const& mapping : node.event_inputs) {
      if (mapping.node_bundle_ports.empty()) continue;
      result.event_inputs.push_back({
          .id = {node.id, PortKind::event, mapping.index},
          .config = bundles.resolve_event_input(mapping.node_bundle_ports.front()).config,
          .node_bundle_ports = mapping.node_bundle_ports});
    }
    for (auto const& mapping : node.event_outputs) {
      if (mapping.node_bundle_ports.empty()) continue;
      result.event_outputs.push_back({
          .id = {node.id, PortKind::event, mapping.index},
          .config = bundles.resolve_event_output(mapping.node_bundle_ports.front()).config,
          .node_bundle_ports = mapping.node_bundle_ports});
    }
  }
  return result;
}

constexpr std::vector<std::string> GraphBuilderSymbolicNodes::ids_for_bundle(
    NodeBundle const& bundle) const {
  std::vector<std::string> ids;
  for (auto const handle : bundle.symbolic_node_handles()) ids.push_back(record(handle).id);
  return ids;
}
} // namespace iv
