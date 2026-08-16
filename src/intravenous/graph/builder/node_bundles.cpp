#include <intravenous/graph/builder/node_bundles.h>

#include <intravenous/graph/builder/topology.h>

#include <type_traits>

namespace iv {
NodeBundleHandle GraphBuilderNodeBundles::append_concrete(
    GraphBuilderTopology const &topology, size_t concrete_node_index) {
  auto const &concrete_node = topology.concrete_node(concrete_node_index);
  NodeBundle bundle{.concrete_node_ids = {concrete_node_index}};
  bundle.sample_inputs.reserve(concrete_node.inputs().size());
  for (size_t ordinal = 0; ordinal < concrete_node.inputs().size(); ++ordinal) {
    bundle.sample_inputs.emplace_back(ConcreteSamplePortMapping{
        .concrete_port = PortId{concrete_node_index, ordinal},
        .channel_layout = concrete_node.inputs()[ordinal].channel_layout,
    });
  }
  bundle.sample_outputs.reserve(concrete_node.outputs().size());
  for (size_t ordinal = 0; ordinal < concrete_node.outputs().size(); ++ordinal) {
    bundle.sample_outputs.emplace_back(ConcreteSamplePortMapping{
        .concrete_port = PortId{concrete_node_index, ordinal},
        .channel_layout = concrete_node.outputs()[ordinal].channel_layout,
    });
  }
  bundle.event_inputs.reserve(concrete_node.event_inputs().size());
  for (size_t ordinal = 0; ordinal < concrete_node.event_inputs().size(); ++ordinal) {
    bundle.event_inputs.emplace_back(ConcreteEventPortMapping{
        .concrete_port = PortId{concrete_node_index, ordinal},
        .type = concrete_node.event_inputs()[ordinal].type,
    });
  }
  bundle.event_outputs.reserve(concrete_node.event_outputs().size());
  for (size_t ordinal = 0; ordinal < concrete_node.event_outputs().size(); ++ordinal) {
    bundle.event_outputs.emplace_back(ConcreteEventPortMapping{
        .concrete_port = PortId{concrete_node_index, ordinal},
        .type = concrete_node.event_outputs()[ordinal].type,
    });
  }
  auto const handle = _bundles.size();
  _bundles.push_back(std::move(bundle));
  if (_bundle_by_concrete_node.size() <= concrete_node_index) {
    _bundle_by_concrete_node.resize(concrete_node_index + 1, GRAPH_ID);
  }
  _bundle_by_concrete_node[concrete_node_index] = handle;
  return handle;
}

NodeBundle const &GraphBuilderNodeBundles::bundle(NodeBundleHandle handle) const {
  return _bundles.at(handle);
}

NodeBundle &GraphBuilderNodeBundles::bundle(NodeBundleHandle handle) {
  return _bundles.at(handle);
}

size_t GraphBuilderNodeBundles::concrete_node_index(
    NodeBundleHandle handle) const {
  auto const &value = bundle(handle);
  if (value.subgraph_node_id || value.concrete_node_ids.size() != 1) {
    details::error("this operation requires a NodeBundle with one ConcreteNode");
  }
  return value.concrete_node_ids.front();
}

size_t GraphBuilderNodeBundles::single_node_index(
    NodeBundleHandle handle) const {
  auto const &value = bundle(handle);
  if (value.subgraph_node_id) {
    return *value.subgraph_node_id;
  }
  if (value.concrete_node_ids.size() == 1) {
    return value.concrete_node_ids.front();
  }
  details::error("this operation requires a NodeBundle with one node");
}

NodeBundleHandle GraphBuilderNodeBundles::append_subgraph(
    GraphBuilderTopology const &topology, size_t subgraph_node_index) {
  auto const &subgraph = topology.subgraph_node(subgraph_node_index);
  NodeBundle bundle{.subgraph_node_id = subgraph_node_index};
  for (size_t ordinal = 0; ordinal < subgraph.inputs().size(); ++ordinal) {
    bundle.sample_inputs.emplace_back(SubgraphSamplePortMapping{
        .subgraph_port = PortId{subgraph_node_index, ordinal},
        .channel_layout = subgraph.inputs()[ordinal].channel_layout,
    });
  }
  for (size_t ordinal = 0; ordinal < subgraph.outputs().size(); ++ordinal) {
    bundle.sample_outputs.emplace_back(SubgraphSamplePortMapping{
        .subgraph_port = PortId{subgraph_node_index, ordinal},
        .channel_layout = subgraph.outputs()[ordinal].channel_layout,
    });
  }
  for (size_t ordinal = 0; ordinal < subgraph.event_inputs().size(); ++ordinal) {
    bundle.event_inputs.emplace_back(SubgraphEventPortMapping{
        .subgraph_port = PortId{subgraph_node_index, ordinal},
        .type = subgraph.event_inputs()[ordinal].type,
    });
  }
  for (size_t ordinal = 0; ordinal < subgraph.event_outputs().size(); ++ordinal) {
    bundle.event_outputs.emplace_back(SubgraphEventPortMapping{
        .subgraph_port = PortId{subgraph_node_index, ordinal},
        .type = subgraph.event_outputs()[ordinal].type,
    });
  }
  auto const handle = _bundles.size();
  _bundles.push_back(std::move(bundle));
  if (_bundle_by_concrete_node.size() <= subgraph_node_index) {
    _bundle_by_concrete_node.resize(subgraph_node_index + 1, GRAPH_ID);
  }
  _bundle_by_concrete_node[subgraph_node_index] = handle;
  return handle;
}

NodeBundleHandle GraphBuilderNodeBundles::bundle_for_concrete_node(
    size_t concrete_node_index) const {
  if (concrete_node_index >= _bundle_by_concrete_node.size() ||
      _bundle_by_concrete_node[concrete_node_index] == GRAPH_ID) {
    details::error("concrete node has no NodeBundle");
  }
  return _bundle_by_concrete_node[concrete_node_index];
}

size_t GraphBuilderNodeBundles::size() const { return _bundles.size(); }

void GraphBuilderNodeBundles::import_child(
    GraphBuilderNodeBundles const &child, size_t concrete_node_offset) {
  for (auto bundle : child._bundles) {
    // Virtual-node handles are local to their registry. The parent registry
    // rebuilds these inverse links while importing VirtualNode records.
    bundle.virtual_node_handles.clear();
    for (auto &node_id : bundle.concrete_node_ids) {
      node_id += concrete_node_offset;
    }
    if (bundle.subgraph_node_id) {
      *bundle.subgraph_node_id += concrete_node_offset;
    }
    auto offset_sample_ports = [concrete_node_offset](auto &ports) {
      for (auto &port : ports) {
        std::visit([concrete_node_offset](auto &mapping) {
          if constexpr (std::is_same_v<std::remove_cvref_t<decltype(mapping)>,
                                       ConcreteSamplePortMapping>) {
            mapping.concrete_port.node += concrete_node_offset;
          } else if constexpr (std::is_same_v<std::remove_cvref_t<decltype(mapping)>,
                                              TiledSamplePortMapping>) {
            for (auto &channel : mapping.channel_ports) {
              channel.concrete_port.node += concrete_node_offset;
            }
          } else {
            mapping.subgraph_port.node += concrete_node_offset;
          }
        }, port);
      }
    };
    offset_sample_ports(bundle.sample_inputs);
    offset_sample_ports(bundle.sample_outputs);
    auto offset_event_ports = [concrete_node_offset](auto &ports) {
      for (auto &mapping : ports) {
        std::visit([concrete_node_offset](auto &value) {
          using Mapping = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<Mapping, ConcreteEventPortMapping>) {
            value.concrete_port.node += concrete_node_offset;
          } else {
            value.subgraph_port.node += concrete_node_offset;
          }
        }, mapping);
      }
    };
    offset_event_ports(bundle.event_inputs);
    offset_event_ports(bundle.event_outputs);
    auto const handle = _bundles.size();
    _bundles.push_back(std::move(bundle));
    std::vector<size_t> topology_node_indices;
    for (auto const node_id : _bundles.back().concrete_node_ids) {
      topology_node_indices.push_back(node_id);
    }
    if (_bundles.back().subgraph_node_id) {
      topology_node_indices.push_back(*_bundles.back().subgraph_node_id);
    }
    for (auto const node_index : topology_node_indices) {
      if (_bundle_by_concrete_node.size() <= node_index) {
        _bundle_by_concrete_node.resize(node_index + 1, GRAPH_ID);
      }
      _bundle_by_concrete_node[node_index] = handle;
    }
  }
}
} // namespace iv
