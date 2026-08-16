#include <intravenous/graph/builder/virtual_nodes.h>

#include <intravenous/graph/builder/topology.h>

#include <algorithm>

namespace iv {
namespace {
template <class Config>
void append_sample_port_mappings(
    std::vector<VirtualSamplePortMapping> &mappings,
    std::vector<Config> const &configs, size_t node_index) {
  if (mappings.empty()) {
    mappings.reserve(configs.size());
    for (size_t ordinal = 0; ordinal < configs.size(); ++ordinal) {
      mappings.push_back(VirtualSamplePortMapping{
          .name = configs[ordinal].name,
          .ordinal = ordinal,
          .channel_layout = configs[ordinal].channel_layout,
          .concrete_ports = {PortId{node_index, ordinal}},
      });
    }
    return;
  }
  if (mappings.size() != configs.size()) {
    details::error(
        "virtual node members must expose the same number of sample ports");
  }
  for (size_t ordinal = 0; ordinal < configs.size(); ++ordinal) {
    auto &mapping = mappings[ordinal];
    if (mapping.name != configs[ordinal].name ||
        mapping.channel_layout != configs[ordinal].channel_layout) {
      details::error("virtual node members must expose matching sample port "
                     "configurations");
    }
    mapping.concrete_ports.push_back(PortId{node_index, ordinal});
  }
}

template <class Config>
void append_event_port_mappings(std::vector<VirtualEventPortMapping> &mappings,
                                std::vector<Config> const &configs,
                                size_t node_index) {
  if (mappings.empty()) {
    mappings.reserve(configs.size());
    for (size_t ordinal = 0; ordinal < configs.size(); ++ordinal) {
      mappings.push_back(VirtualEventPortMapping{
          .name = configs[ordinal].name,
          .ordinal = ordinal,
          .type = configs[ordinal].type,
          .concrete_ports = {PortId{node_index, ordinal}},
      });
    }
    return;
  }
  if (mappings.size() != configs.size()) {
    details::error(
        "virtual node members must expose the same number of event ports");
  }
  for (size_t ordinal = 0; ordinal < configs.size(); ++ordinal) {
    auto &mapping = mappings[ordinal];
    if (mapping.name != configs[ordinal].name ||
        mapping.type != configs[ordinal].type) {
      details::error("virtual node members must expose matching event port "
                     "configurations");
    }
    mapping.concrete_ports.push_back(PortId{node_index, ordinal});
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
  auto const concrete_node_index =
      node_bundles.single_node_index(node_bundle_handle);
  if (!std::ranges::contains(virtual_node.node_bundle_handles,
                             node_bundle_handle)) {
    virtual_node.node_bundle_handles.push_back(node_bundle_handle);
  }
  // A virtual node may name a SubgraphNode bundle. Its membership is fully
  // represented by node_bundle_handles; the concrete-port projection below is
  // only meaningful for a non-tiled ConcreteNode bundle.
  if (topology.is_subgraph_node(concrete_node_index)) {
    auto &bundle_handles =
        node_bundles.bundle(node_bundle_handle).virtual_node_handles;
    if (!std::ranges::contains(bundle_handles, handle)) {
      bundle_handles.push_back(handle);
    }
    if (source_info &&
        std::find(virtual_node.source_infos.begin(),
                  virtual_node.source_infos.end(), *source_info) ==
            virtual_node.source_infos.end()) {
      virtual_node.source_infos.push_back(*source_info);
    }
    return;
  }
  if (!std::ranges::contains(virtual_node.concrete_node_indices,
                             concrete_node_index)) {
    virtual_node.concrete_node_indices.push_back(concrete_node_index);
    auto const &concrete_node = topology.concrete_node(concrete_node_index);
    append_sample_port_mappings(virtual_node.sample_inputs,
                                concrete_node.inputs(), concrete_node_index);
    append_sample_port_mappings(virtual_node.sample_outputs,
                                concrete_node.outputs(), concrete_node_index);
    append_event_port_mappings(virtual_node.event_inputs,
                               concrete_node.event_inputs(),
                               concrete_node_index);
    append_event_port_mappings(virtual_node.event_outputs,
                               concrete_node.event_outputs(),
                               concrete_node_index);
  }
  auto &bundle_handles = node_bundles.bundle(node_bundle_handle).virtual_node_handles;
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
    auto offset_ports = [node_offset](auto &ports) {
      for (auto &port : ports) {
        for (auto &endpoint : port.concrete_ports)
          endpoint.node += node_offset;
      }
    };
    offset_ports(mapping.sample_inputs);
    offset_ports(mapping.sample_outputs);
    offset_ports(mapping.event_inputs);
    offset_ports(mapping.event_outputs);
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

std::vector<std::string>
GraphBuilderVirtualNodes::ids_for_bundle(NodeBundle const &bundle) const {
  std::vector<std::string> ids;
  ids.reserve(bundle.virtual_node_handles.size());
  for (auto const handle : bundle.virtual_node_handles) {
    ids.push_back(record(handle).id);
  }
  return ids;
}
} // namespace iv
