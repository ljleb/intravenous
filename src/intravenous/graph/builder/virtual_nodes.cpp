#include <intravenous/graph/builder/virtual_nodes.h>

#include <intravenous/graph/builder/topology.h>

#include <algorithm>

namespace iv {
namespace {
template <class Config>
void append_virtual_event_port_mapping(
    std::vector<VirtualEventPortMapping> &mappings, Config const &config,
    size_t ordinal, NodeBundlePortId bundle_port) {
  if (mappings.size() <= ordinal) mappings.resize(ordinal + 1);
  auto &mapping = mappings[ordinal];
  if (mapping.node_bundle_ports.empty()) {
    mapping = VirtualEventPortMapping{.name = config.name, .ordinal = ordinal,
                                      .type = config.type,
                                      .node_bundle_ports = {bundle_port}};
    return;
  }
  if (mapping.name != config.name || mapping.type != config.type) {
    details::error("virtual node members must expose matching event port configurations");
  }
  if (!std::ranges::contains(mapping.node_bundle_ports, bundle_port)) {
    mapping.node_bundle_ports.push_back(bundle_port);
  }
}

void append_bundle_event_port_mappings(
    std::vector<VirtualEventPortMapping> &mappings, NodeBundle const &bundle,
    NodeBundleHandle bundle_handle, GraphBuilderTopology const &topology,
    bool inputs) {
  (void)topology;
  auto const count = inputs ? bundle.event_input_count() : bundle.event_output_count();
  for (size_t ordinal = 0; ordinal < count; ++ordinal) {
    auto append = [&](auto const &config) {
      append_virtual_event_port_mapping(mappings, config, ordinal,
                                        {bundle_handle, PortKind::event, ordinal});
    };
    if (inputs) append(bundle.event_input_descriptor(ordinal).config);
    else append(bundle.event_output_descriptor(ordinal).config);
  }
}

template <class Config>
void append_virtual_sample_port_mapping(
    std::vector<VirtualSamplePortMapping> &mappings, Config const &config,
    size_t ordinal, ChannelLayout layout, NodeBundlePortId bundle_port) {
  if (mappings.size() <= ordinal) {
    mappings.resize(ordinal + 1);
  }
  auto &mapping = mappings[ordinal];
  if (mapping.node_bundle_ports.empty()) {
    mapping = VirtualSamplePortMapping{
        .name = config.name,
        .ordinal = ordinal,
        .channel_layout = layout,
        .node_bundle_ports = {bundle_port},
    };
    return;
  }
  if (mapping.name != config.name || mapping.channel_layout != layout) {
    details::error("virtual node members must expose matching sample port configurations");
  }
  if (!std::ranges::contains(mapping.node_bundle_ports, bundle_port)) {
    mapping.node_bundle_ports.push_back(bundle_port);
  }
}

void append_bundle_sample_port_mappings(
    std::vector<VirtualSamplePortMapping> &mappings,
    NodeBundle const &bundle, NodeBundleHandle bundle_handle,
    GraphBuilderTopology const &topology, bool inputs) {
  (void)topology;
  auto const count = inputs ? bundle.sample_input_count() : bundle.sample_output_count();
  for (size_t ordinal = 0; ordinal < count; ++ordinal) {
    auto append = [&](auto const &config) {
      append_virtual_sample_port_mapping(mappings, config, ordinal,
          config.channel_layout,
          {bundle_handle, PortKind::sample, ordinal});
    };
    if (inputs) append(bundle.sample_input_descriptor(ordinal).config);
    else append(bundle.sample_output_descriptor(ordinal).config);
  }
}
} // namespace

VirtualNodeHandle
GraphBuilderVirtualNodes::get_or_create(std::string_view virtual_node_id) {
  auto const it = _handles_by_id.find(std::string(virtual_node_id));
  if (it != _handles_by_id.end()) {
    return it->second;
  }
  auto const handle = _records.size();
  _records.push_back(VirtualNodeRecord{.id = std::string(virtual_node_id)});
  _handles_by_id.emplace(_records.back().id, handle);
  return handle;
}

void GraphBuilderVirtualNodes::attach_member(GraphBuilderTopology &topology,
                                             GraphBuilderNodeBundles &node_bundles,
                                             VirtualNodeHandle handle,
                                             NodeBundleHandle node_bundle_handle,
                                             SourceInfo const *source_info) {
  auto &virtual_node = _records[handle];
  auto const &bundle = node_bundles.bundle(node_bundle_handle);
  if (!std::ranges::contains(virtual_node.node_bundle_handles,
                             node_bundle_handle)) {
    virtual_node.node_bundle_handles.push_back(node_bundle_handle);
  }
  bool has_new_concrete_member = false;
  bundle.for_each_concrete_node([&](size_t concrete_node_index) {
    if (!std::ranges::contains(virtual_node.concrete_node_indices,
                               concrete_node_index)) {
      virtual_node.concrete_node_indices.push_back(concrete_node_index);
      has_new_concrete_member = true;
    }
  });
  if (has_new_concrete_member) {
    append_bundle_sample_port_mappings(virtual_node.sample_inputs, bundle,
                                       node_bundle_handle, topology, true);
    append_bundle_sample_port_mappings(virtual_node.sample_outputs, bundle,
                                       node_bundle_handle, topology, false);
    append_bundle_event_port_mappings(virtual_node.event_inputs, bundle,
                                      node_bundle_handle, topology, true);
    append_bundle_event_port_mappings(virtual_node.event_outputs, bundle,
                                      node_bundle_handle, topology, false);
  }
  auto &bundle_handles = node_bundles.bundle(node_bundle_handle).virtual_node_handles();
  if (!std::ranges::contains(bundle_handles, handle)) {
    bundle_handles.push_back(handle);
  }
  if (source_info && std::find(virtual_node.source_infos.begin(),
                               virtual_node.source_infos.end(), *source_info) ==
                         virtual_node.source_infos.end()) {
    virtual_node.source_infos.push_back(*source_info);
  }
}

void GraphBuilderVirtualNodes::attach_bundle_member(
    GraphBuilderTopology &topology, GraphBuilderNodeBundles &node_bundles,
    NodeBundleHandle node_bundle_handle,
    std::string_view virtual_node_id, SourceInfo const *source_info) {
  if (virtual_node_id.empty())
    return;
  attach_member(topology, node_bundles, get_or_create(virtual_node_id),
                node_bundle_handle, source_info);
}

void GraphBuilderVirtualNodes::attach_tiled_members(
    GraphBuilderTopology &topology, GraphBuilderNodeBundles &node_bundles,
    VirtualNodeRecord mapping,
    SourceInfo const *source_info) {
  if (mapping.id.empty())
    return;
  auto const handle = get_or_create(mapping.id);
  auto &record = _records[handle];
  for (auto const node_bundle_handle : mapping.node_bundle_handles) {
    attach_member(topology, node_bundles, handle, node_bundle_handle, source_info);
  }
  // Imported non-tiled members are merged above by attach_member(), which
  // validates their port configurations and appends their lowered ports.
  // Only preserve an explicit mapping when there are no concrete members to
  // derive it from (the currently-unimplemented tiled-node case).
  if (record.concrete_node_indices.empty()) {
    record.sample_inputs = std::move(mapping.sample_inputs);
    record.sample_outputs = std::move(mapping.sample_outputs);
    record.event_inputs = std::move(mapping.event_inputs);
    record.event_outputs = std::move(mapping.event_outputs);
  }
}

void GraphBuilderVirtualNodes::import_child(
    GraphBuilderTopology &topology, GraphBuilderNodeBundles &node_bundles,
    GraphBuilderVirtualNodes const &child,
    size_t node_offset, size_t node_bundle_offset) {
  for (auto const &child_record : child.records()) {
    auto mapping = child_record;
    for (auto &handle : mapping.node_bundle_handles)
      handle += node_bundle_offset;
    for (auto &node_index : mapping.concrete_node_indices)
      node_index += node_offset;
    auto offset_sample_ports = [node_bundle_offset](auto &ports) {
      for (auto &port : ports) {
        for (auto &endpoint : port.node_bundle_ports)
          endpoint.node_bundle_handle += node_bundle_offset;
      }
    };
    auto offset_event_ports = [node_bundle_offset](auto &ports) {
      for (auto &port : ports) {
        for (auto &endpoint : port.node_bundle_ports)
          endpoint.node_bundle_handle += node_bundle_offset;
      }
    };
    offset_sample_ports(mapping.sample_inputs);
    offset_sample_ports(mapping.sample_outputs);
    offset_event_ports(mapping.event_inputs);
    offset_event_ports(mapping.event_outputs);
    attach_tiled_members(topology, node_bundles, std::move(mapping));
    auto &record = _records[_handles_by_id.at(child_record.id)];
    for (auto const &info : child_record.source_infos) {
      if (std::find(record.source_infos.begin(), record.source_infos.end(),
                    info) == record.source_infos.end()) {
        record.source_infos.push_back(info);
      }
    }
  }
}

std::vector<VirtualNodeRecord> const &
GraphBuilderVirtualNodes::records() const {
  return _records;
}
VirtualNodeRecord const &
GraphBuilderVirtualNodes::record(VirtualNodeHandle handle) const {
  return _records.at(handle);
}

GraphBuilderVirtualPorts GraphBuilderVirtualNodes::ports(
    GraphBuilderTopology const &topology,
    GraphBuilderNodeBundles const &node_bundles) const {
  (void)topology;
  GraphBuilderVirtualPorts result;
  auto input_config = [&](NodeBundlePortId id) -> InputConfig {
    return node_bundles.resolve_sample_input(id).config;
  };
  auto output_config = [&](NodeBundlePortId id) -> OutputConfig {
    return node_bundles.resolve_sample_output(id).config;
  };
  auto event_input_config = [&](NodeBundlePortId id) -> EventInputConfig {
    return node_bundles.resolve_event_input(id).config;
  };
  auto event_output_config = [&](NodeBundlePortId id) -> EventOutputConfig {
    return node_bundles.resolve_event_output(id).config;
  };
  for (auto const &node : _records) {
    for (auto const &mapping : node.sample_inputs) {
      if (!mapping.node_bundle_ports.empty()) result.sample_inputs.push_back({
          .id = {node.id, PortKind::sample, mapping.ordinal},
          .config = input_config(mapping.node_bundle_ports.front()),
          .node_bundle_ports = mapping.node_bundle_ports});
      if (!mapping.node_bundle_ports.empty())
        result.sample_inputs.back().config.channel_layout = mapping.channel_layout;
    }
    for (auto const &mapping : node.sample_outputs) {
      if (!mapping.node_bundle_ports.empty()) result.sample_outputs.push_back({
          .id = {node.id, PortKind::sample, mapping.ordinal},
          .config = output_config(mapping.node_bundle_ports.front()),
          .node_bundle_ports = mapping.node_bundle_ports});
      if (!mapping.node_bundle_ports.empty())
        result.sample_outputs.back().config.channel_layout = mapping.channel_layout;
    }
    for (auto const &mapping : node.event_inputs) {
      if (mapping.node_bundle_ports.empty()) continue;
      result.event_inputs.push_back({.id = {node.id, PortKind::event, mapping.ordinal},
          .config = event_input_config(mapping.node_bundle_ports.front()),
          .node_bundle_ports = mapping.node_bundle_ports});
    }
    for (auto const &mapping : node.event_outputs) {
      if (mapping.node_bundle_ports.empty()) continue;
      result.event_outputs.push_back({.id = {node.id, PortKind::event, mapping.ordinal},
          .config = event_output_config(mapping.node_bundle_ports.front()),
          .node_bundle_ports = mapping.node_bundle_ports});
    }
  }
  return result;
}

std::vector<std::string>
GraphBuilderVirtualNodes::ids_for_bundle(NodeBundle const &bundle) const {
  std::vector<std::string> ids;
  ids.reserve(bundle.virtual_node_handles().size());
  for (auto const handle : bundle.virtual_node_handles()) {
    ids.push_back(record(handle).id);
  }
  return ids;
}
} // namespace iv
