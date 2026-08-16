#pragma once

#include <intravenous/graph/builder/stored_node.h>

#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

namespace iv {
using NodeBundleHandle = size_t;

struct ConcreteSamplePortMapping {
  PortId concrete_port{};
  ChannelLayout channel_layout{};
};

// These are topology storage indices, not additional node types. The node
// types themselves are declared in stored_node.h.
using ConcreteNodeId = size_t;
using SubgraphNodeId = size_t;

struct TiledSamplePortChannelMapping {
  size_t channel_ordinal = 0;
  PortId concrete_port{};
};

struct TiledSamplePortMapping {
  ChannelLayout channel_layout{};
  std::vector<TiledSamplePortChannelMapping> channel_ports{};
};

struct SubgraphSamplePortMapping {
  PortId subgraph_port{};
  ChannelLayout channel_layout{};
};

using NodeBundleSamplePortMapping =
    std::variant<ConcreteSamplePortMapping, TiledSamplePortMapping,
                 SubgraphSamplePortMapping>;

struct ConcreteEventPortMapping {
  PortId concrete_port{};
  EventTypeId type = EventTypeId::empty;
};

struct SubgraphEventPortMapping {
  PortId subgraph_port{};
  EventTypeId type = EventTypeId::empty;
};

using NodeBundleEventPortMapping =
    std::variant<ConcreteEventPortMapping, SubgraphEventPortMapping>;

// A NodeBundle is the uniform builder-facing representation of a node. A
// A bundle containing one ConcreteNode has one concrete port for each exposed
// port. A tiled bundle uses the same record but replaces the concrete sample
// mappings with explicit channel-member mappings.
struct NodeBundle {
  std::vector<ConcreteNodeId> concrete_node_ids{};
  std::optional<SubgraphNodeId> subgraph_node_id{};
  std::vector<NodeBundleSamplePortMapping> sample_inputs{};
  std::vector<NodeBundleSamplePortMapping> sample_outputs{};
  std::vector<NodeBundleEventPortMapping> event_inputs{};
  std::vector<NodeBundleEventPortMapping> event_outputs{};
  std::vector<size_t> virtual_node_handles{};
  NodeSourceAnnotations source_annotations{};
};

class GraphBuilderTopology;

class GraphBuilderNodeBundles {
public:
  NodeBundleHandle append_concrete(GraphBuilderTopology const &,
                                   size_t concrete_node_index);
  NodeBundleHandle append_subgraph(GraphBuilderTopology const &,
                                   size_t subgraph_node_index);
  NodeBundle const &bundle(NodeBundleHandle) const;
  NodeBundle &bundle(NodeBundleHandle);
  size_t concrete_node_index(NodeBundleHandle) const;
  size_t single_node_index(NodeBundleHandle) const;
  NodeBundleHandle bundle_for_concrete_node(size_t concrete_node_index) const;
  size_t size() const;

  void import_child(GraphBuilderNodeBundles const &, size_t concrete_node_offset);

private:
  std::vector<NodeBundle> _bundles{};
  std::vector<size_t> _bundle_by_concrete_node{};
};
} // namespace iv
