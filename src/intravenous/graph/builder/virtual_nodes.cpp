#include <intravenous/graph/builder/virtual_nodes.h>

#include <algorithm>
#include <ranges>

namespace iv {
namespace {
template <class Config>
void append_virtual_event_port_mapping(
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
void append_virtual_sample_port_mapping(
    std::vector<Mapping>& mappings, Config const& config, size_t ordinal,
    ChannelLayout layout, Channels const& channels) {
  if (mappings.size() <= ordinal) mappings.resize(ordinal + 1);
  auto& mapping = mappings[ordinal];
  if (mapping.channels.empty()) {
    mapping = {.name = config.name, .ordinal = ordinal,
               .channel_layout = layout,
               .channels = {channels.begin(), channels.end()}};
    return;
  }
  if (mapping.name != config.name || mapping.channel_layout != layout)
    details::error("virtual node members must expose matching sample port configurations");
  for (auto const channel : channels)
    if (!std::ranges::contains(mapping.channels, channel))
      mapping.channels.push_back(channel);
}

void append_bundle_mappings(VirtualNodeRecord& virtual_node,
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

VirtualNodeHandle GraphBuilderVirtualNodes::get_or_create(
    std::string_view virtual_node_id) {
  auto const it = _handles_by_id.find(std::string(virtual_node_id));
  if (it != _handles_by_id.end()) return it->second;
  auto const handle = _records.size();
  _records.push_back({.id = std::string(virtual_node_id)});
  _handles_by_id.emplace(_records.back().id, handle);
  return handle;
}

void GraphBuilderVirtualNodes::attach_member(
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

void GraphBuilderVirtualNodes::attach_bundle_member(
    GraphBuilderNodeBundles& bundles, NodeBundleHandle bundle_handle,
    std::string_view virtual_node_id, SourceInfo const* source_info) {
  if (virtual_node_id.empty()) return;
  attach_member(bundles, get_or_create(virtual_node_id), bundle_handle, source_info);
}

void GraphBuilderVirtualNodes::import_child(
    GraphBuilderNodeBundles& bundles, GraphBuilderVirtualNodes const& child,
    size_t bundle_offset) {
  for (auto const& child_record : child.records()) {
    auto const handle = get_or_create(child_record.id);
    auto& record = _records[handle];
    for (auto child_bundle : child_record.node_bundle_handles)
      attach_member(bundles, handle, child_bundle + bundle_offset, nullptr);
    for (auto const& info : child_record.source_infos)
      if (!std::ranges::contains(record.source_infos, info))
        record.source_infos.push_back(info);
  }
}

std::vector<VirtualNodeRecord> const& GraphBuilderVirtualNodes::records() const {
  return _records;
}
VirtualNodeRecord const& GraphBuilderVirtualNodes::record(VirtualNodeHandle handle) const {
  return _records.at(handle);
}

GraphBuilderVirtualPorts GraphBuilderVirtualNodes::ports(
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

std::vector<std::string> GraphBuilderVirtualNodes::ids_for_bundle(
    NodeBundle const& bundle) const {
  std::vector<std::string> ids;
  for (auto const handle : bundle.virtual_node_handles()) ids.push_back(record(handle).id);
  return ids;
}
} // namespace iv
