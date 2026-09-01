#pragma once

#include <intravenous/graph/builder/stored_node.hpp>
#include <intravenous/graph/builder/node_bundles.hpp>

#include <algorithm>
#include <cstddef>
#include <flat_map>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iv {
using VirtualNodeHandle = size_t;

template<class ChannelId>
struct VirtualSamplePortMapping {
  std::string name{};
  size_t ordinal = 0;
  ChannelLayout channel_layout{};
  std::vector<ChannelId> channels{};
  std::vector<std::vector<ChannelId>> member_channels{};
  bool operator==(VirtualSamplePortMapping const&) const = default;
};
using VirtualSampleInputPortMapping = VirtualSamplePortMapping<SampleInputChannelId>;
using VirtualSampleOutputPortMapping = VirtualSamplePortMapping<SampleOutputChannelId>;

struct VirtualEventPortMapping {
  std::string name{};
  size_t ordinal = 0;
  EventTypeId type = EventTypeId::empty;
  std::vector<NodeBundlePortId> node_bundle_ports{};
  bool operator==(VirtualEventPortMapping const&) const = default;
};

struct VirtualNodeRecord {
  std::string id{};
  std::string source_identity {};
  std::string type_identity {};
  std::vector<SourceInfo> source_infos{};
  std::vector<NodeBundleHandle> node_bundle_handles{};
  std::vector<VirtualSampleInputPortMapping> sample_inputs{};
  std::vector<VirtualSampleOutputPortMapping> sample_outputs{};
  std::vector<VirtualEventPortMapping> event_inputs{};
  std::vector<VirtualEventPortMapping> event_outputs{};
};

struct GraphBuilderVirtualSampleInputPort {
  VirtualPortId id{};
  InputConfig config{};
  std::vector<SampleInputChannelId> channels{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderVirtualSampleOutputPort {
  VirtualPortId id{};
  OutputConfig config{};
  std::vector<SampleOutputChannelId> channels{};
  std::vector<std::vector<SampleOutputChannelId>> member_channels{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderVirtualEventInputPort {
  VirtualPortId id{};
  EventInputConfig config{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderVirtualEventOutputPort {
  VirtualPortId id{};
  EventOutputConfig config{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderVirtualPorts {
  std::vector<GraphBuilderVirtualSampleInputPort> sample_inputs{};
  std::vector<GraphBuilderVirtualSampleOutputPort> sample_outputs{};
  std::vector<GraphBuilderVirtualEventInputPort> event_inputs{};
  std::vector<GraphBuilderVirtualEventOutputPort> event_outputs{};
};

class GraphBuilderVirtualNodes {
public:
  constexpr void attach_bundle_member(
      GraphBuilderNodeBundles&, NodeBundleHandle,
      std::string_view virtual_node_id,
      SourceInfo const* source_info = nullptr);
  constexpr void attach_sample_output(
      GraphBuilderNodeBundles&, ChannelTypeId,
      std::span<SampleOutputChannelId const>,
      std::string_view virtual_node_id, SourceInfo const&);
  constexpr void attach_event_output(
      GraphBuilderNodeBundles&, EventTypeId,
      std::span<EventOutputPortId const>,
      std::string_view virtual_node_id, SourceInfo const&);
  constexpr void import_child(
      GraphBuilderNodeBundles&, GraphBuilderVirtualNodes const&,
      size_t node_bundle_offset);

  constexpr std::vector<VirtualNodeRecord> const& records() const;
  static constexpr GraphBuilderVirtualNodes from_authored_records(
      std::span<VirtualNodeRecord const>);
  constexpr GraphBuilderVirtualPorts ports(
      GraphBuilderNodeBundles const&) const;
  constexpr VirtualNodeRecord const& record(VirtualNodeHandle) const;
  constexpr std::vector<std::string> ids_for_bundle(NodeBundle const&) const;

private:
    constexpr VirtualNodeHandle get_or_create(
        std::string_view source_identity, std::string_view type_identity);
    constexpr void attach_member(
        GraphBuilderNodeBundles&, VirtualNodeHandle, NodeBundleHandle,
        SourceInfo const*);

    std::vector<VirtualNodeRecord> _records {};
    std::flat_map<std::string, std::vector<VirtualNodeHandle>>
        _handles_by_source_identity {};
};
} // namespace iv
namespace iv {
namespace {
template <class Config>
constexpr void append_virtual_event_port_mapping(
    std::vector<VirtualEventPortMapping>& mappings, Config const& config,
    size_t ordinal, NodeBundlePortId bundle_port) {
  if (mappings.size() <= ordinal) mappings.resize(ordinal + 1);
  auto& mapping = mappings[ordinal];
  if (mapping.node_bundle_ports.empty()) {
    mapping = {.name = config.name, .ordinal = ordinal, .type = config.type,
               .node_bundle_ports = {bundle_port}};
    return;
  }
  if (mapping.name != config.name || mapping.type != config.type)
    details::error("virtual node members must expose matching event port configurations");
  if (!std::ranges::contains(mapping.node_bundle_ports, bundle_port))
    mapping.node_bundle_ports.push_back(bundle_port);
}

template<class Mapping, class Config, class Channels>
constexpr void append_virtual_sample_port_mapping(
    std::vector<Mapping>& mappings, Config const& config, size_t ordinal,
    ChannelLayout layout, Channels const& channels) {
  if (mappings.size() <= ordinal) mappings.resize(ordinal + 1);
  auto& mapping = mappings[ordinal];
  if (mapping.channels.empty()) {
    mapping = {.name = config.name, .ordinal = ordinal,
               .channel_layout = layout,
               .channels = {channels.begin(), channels.end()},
               .member_channels = {{channels.begin(), channels.end()}}};
    return;
  }
  if (mapping.name != config.name || mapping.channel_layout != layout)
    details::error("virtual node members must expose matching sample port configurations");
  for (auto const channel : channels)
    if (!std::ranges::contains(mapping.channels, channel))
      mapping.channels.push_back(channel);
  mapping.member_channels.emplace_back(channels.begin(), channels.end());
}

constexpr void append_bundle_mappings(
    VirtualNodeRecord& virtual_node,
    GraphBuilderNodeBundles const& bundles,
    NodeBundleHandle handle) {
  auto const& bundle = bundles.bundle(handle);
  for (size_t ordinal = 0; ordinal < bundle.sample_input_count(); ++ordinal) {
    NodeBundlePortId const port{handle, PortKind::sample, ordinal};
    auto const config = bundles.resolve_sample_input(port).config;
    append_virtual_sample_port_mapping(virtual_node.sample_inputs, config, ordinal,
                                       config.channel_layout,
                                       bundles.sample_input_channels(port));
  }
  for (size_t ordinal = 0; ordinal < bundle.sample_output_count(); ++ordinal) {
    NodeBundlePortId const port{handle, PortKind::sample, ordinal};
    auto const config = bundles.resolve_sample_output(port).config;
    append_virtual_sample_port_mapping(virtual_node.sample_outputs, config, ordinal,
                                       config.channel_layout,
                                       bundles.sample_output_channels(port));
  }
  for (size_t ordinal = 0; ordinal < bundle.event_input_count(); ++ordinal) {
    NodeBundlePortId const port{handle, PortKind::event, ordinal};
    append_virtual_event_port_mapping(virtual_node.event_inputs,
        bundles.resolve_event_input(port).config, ordinal, port);
  }
  for (size_t ordinal = 0; ordinal < bundle.event_output_count(); ++ordinal) {
    NodeBundlePortId const port{handle, PortKind::event, ordinal};
    append_virtual_event_port_mapping(virtual_node.event_outputs,
        bundles.resolve_event_output(port).config, ordinal, port);
  }
}
} // namespace

constexpr VirtualNodeHandle GraphBuilderVirtualNodes::get_or_create(
    std::string_view source_identity, std::string_view type_identity
)
{
    auto& handles = _handles_by_source_identity[std::string(source_identity)];
    auto const existing = std::find_if(
        handles.begin(), handles.end(), [&](VirtualNodeHandle handle) {
            return _records[handle].type_identity == type_identity;
        }
    );
    if (existing != handles.end())
        return *existing;

    auto const handle = _records.size();
    _records.push_back({
        .id = details::typed_virtual_node_id(source_identity, type_identity),
        .source_identity = std::string(source_identity),
        .type_identity = std::string(type_identity),
    });
    handles.push_back(handle);
    return handle;
}

constexpr void GraphBuilderVirtualNodes::attach_member(
    GraphBuilderNodeBundles& bundles, VirtualNodeHandle virtual_handle,
    NodeBundleHandle bundle_handle, SourceInfo const* source_info) {
  auto& record = _records[virtual_handle];
  if (!std::ranges::contains(record.node_bundle_handles, bundle_handle)) {
    record.node_bundle_handles.push_back(bundle_handle);
    append_bundle_mappings(record, bundles, bundle_handle);
  }
  auto& inverse = bundles.bundle(bundle_handle).virtual_node_handles();
  if (!std::ranges::contains(inverse, virtual_handle)) inverse.push_back(virtual_handle);
  if (source_info && !std::ranges::contains(record.source_infos, *source_info))
    record.source_infos.push_back(*source_info);
}

constexpr void GraphBuilderVirtualNodes::attach_bundle_member(
    GraphBuilderNodeBundles& bundles, NodeBundleHandle bundle_handle,
    std::string_view virtual_node_id, SourceInfo const* source_info) {
  if (virtual_node_id.empty()) return;
  attach_member(
      bundles,
      get_or_create(virtual_node_id, bundles.bundle(bundle_handle).type_identity()),
      bundle_handle,
      source_info
  );
}

constexpr void GraphBuilderVirtualNodes::attach_sample_output(
    GraphBuilderNodeBundles& bundles, ChannelTypeId channel_type,
    std::span<SampleOutputChannelId const> channels,
    std::string_view source_identity, SourceInfo const& source_info) {
  if (source_identity.empty() || channels.empty()) return;
  (void)bundles;
  auto const handle = get_or_create(source_identity, "sample-port");
  auto& record = _records[handle];
  if (!std::ranges::contains(record.source_infos, source_info))
    record.source_infos.push_back(source_info);
  if (record.sample_outputs.empty()) {
    record.sample_outputs.push_back({
        .ordinal = 0,
        .channel_layout = {
            .channel_type = channel_type,
            .sample_layout = SampleStreamLayout::planar,
        },
        .channels = {channels.begin(), channels.end()},
        .member_channels = {{channels.begin(), channels.end()}},
    });
  }
  for (auto channel : channels) {
    if (!std::ranges::contains(record.sample_outputs.front().channels, channel))
      record.sample_outputs.front().channels.push_back(channel);
    if (!std::ranges::contains(record.node_bundle_handles, channel.bundle))
      record.node_bundle_handles.push_back(channel.bundle);
  }
}

constexpr void GraphBuilderVirtualNodes::attach_event_output(
    GraphBuilderNodeBundles& bundles, EventTypeId type,
    std::span<EventOutputPortId const> sources,
    std::string_view source_identity, SourceInfo const& source_info) {
  if (source_identity.empty() || sources.empty()) return;
  (void)bundles;
  auto const handle = get_or_create(source_identity, "event-port");
  auto& record = _records[handle];
  if (!std::ranges::contains(record.source_infos, source_info))
    record.source_infos.push_back(source_info);
  if (record.event_outputs.empty()) {
    record.event_outputs.push_back({
        .ordinal = 0,
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

constexpr void GraphBuilderVirtualNodes::import_child(
    GraphBuilderNodeBundles& bundles, GraphBuilderVirtualNodes const& child,
    size_t bundle_offset) {
  for (auto const& child_record : child.records()) {
      auto const handle = get_or_create(
          child_record.source_identity, child_record.type_identity
      );
      auto& record = _records[handle];
      for (auto child_bundle : child_record.node_bundle_handles)
          attach_member(bundles, handle, child_bundle + bundle_offset, nullptr);
      for (auto const& info : child_record.source_infos)
          if (!std::ranges::contains(record.source_infos, info))
              record.source_infos.push_back(info);
  }
}

constexpr std::vector<VirtualNodeRecord> const&
GraphBuilderVirtualNodes::records() const {
  return _records;
}
constexpr GraphBuilderVirtualNodes
GraphBuilderVirtualNodes::from_authored_records(
    std::span<VirtualNodeRecord const> records) {
  GraphBuilderVirtualNodes result;
  result._records.assign(records.begin(), records.end());
  for (size_t handle = 0; handle < result._records.size(); ++handle) {
    auto const& record = result._records[handle];
    result._handles_by_source_identity[record.source_identity].push_back(handle);
  }
  return result;
}
constexpr VirtualNodeRecord const& GraphBuilderVirtualNodes::record(
    VirtualNodeHandle handle) const {
  return _records.at(handle);
}

constexpr GraphBuilderVirtualPorts GraphBuilderVirtualNodes::ports(
    GraphBuilderNodeBundles const& bundles) const {
  GraphBuilderVirtualPorts result;
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
      config.channel_layout = mapping.channel_layout;
      result.sample_inputs.push_back({
          .id = {node.id, PortKind::sample, mapping.ordinal},
          .config = std::move(config), .channels = mapping.channels,
          .node_bundle_ports = sample_bundle_ports(mapping.channels)});
    }
    for (auto const& mapping : node.sample_outputs) {
      if (mapping.channels.empty()) continue;
      auto const first = mapping.channels.front();
      auto config = bundles.resolve_sample_output(
          {first.bundle, PortKind::sample, first.port}).config;
      config.channel_layout = mapping.channel_layout;
      result.sample_outputs.push_back({
          .id = {node.id, PortKind::sample, mapping.ordinal},
          .config = std::move(config), .channels = mapping.channels,
          .member_channels = mapping.member_channels,
          .node_bundle_ports = sample_bundle_ports(mapping.channels)});
    }
    for (auto const& mapping : node.event_inputs) {
      if (mapping.node_bundle_ports.empty()) continue;
      result.event_inputs.push_back({
          .id = {node.id, PortKind::event, mapping.ordinal},
          .config = bundles.resolve_event_input(mapping.node_bundle_ports.front()).config,
          .node_bundle_ports = mapping.node_bundle_ports});
    }
    for (auto const& mapping : node.event_outputs) {
      if (mapping.node_bundle_ports.empty()) continue;
      result.event_outputs.push_back({
          .id = {node.id, PortKind::event, mapping.ordinal},
          .config = bundles.resolve_event_output(mapping.node_bundle_ports.front()).config,
          .node_bundle_ports = mapping.node_bundle_ports});
    }
  }
  return result;
}

constexpr std::vector<std::string> GraphBuilderVirtualNodes::ids_for_bundle(
    NodeBundle const& bundle) const {
  std::vector<std::string> ids;
  for (auto const handle : bundle.virtual_node_handles()) ids.push_back(record(handle).id);
  return ids;
}
} // namespace iv
