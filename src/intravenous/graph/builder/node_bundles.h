#pragma once

#include <intravenous/graph/builder/stored_node.h>

#include <cstddef>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace iv {
using NodeBundleHandle = size_t;

// An address in the builder-visible interface of one node insertion result.
// It deliberately precedes any lowering to a ConcreteNode or SubgraphNode
// port, so authored virtual ports can retain their structural membership.
struct NodeBundlePortId {
  NodeBundleHandle node_bundle_handle = 0;
  PortKind port_kind = PortKind::sample;
  size_t port_ordinal = 0;
  bool operator==(NodeBundlePortId const &) const = default;
};

struct ConcreteSamplePortMapping {
  ConcretePortId concrete_port{};
  ChannelLayout channel_layout{};
};

// These are topology storage indices, not additional node types. The node
// types themselves are declared in stored_node.h.
using ConcreteNodeId = size_t;
using SubgraphNodeId = size_t;

struct TiledSamplePortChannelMapping {
  size_t channel_ordinal = 0;
  ConcretePortId concrete_port{};
};

struct TiledSamplePortMapping {
  ChannelLayout channel_layout{};
  std::vector<TiledSamplePortChannelMapping> channel_ports{};
};

struct SubgraphSamplePortMapping {
  ConcretePortId subgraph_port{};
  ChannelLayout channel_layout{};
};

using NodeBundleSamplePortMapping =
    std::variant<ConcreteSamplePortMapping, TiledSamplePortMapping,
                 SubgraphSamplePortMapping>;

struct ConcreteEventPortMapping {
  ConcretePortId concrete_port{};
  EventTypeId type = EventTypeId::empty;
};

// Event ports are not channelized. A tiled event port fans one input out to
// every tile, or merges one output from every tile.
struct TiledEventPortMapping {
  EventTypeId type = EventTypeId::empty;
  std::vector<ConcretePortId> concrete_ports{};
};

struct SubgraphEventPortMapping {
  ConcretePortId subgraph_port{};
  EventTypeId type = EventTypeId::empty;
};

using NodeBundleEventPortMapping =
    std::variant<ConcreteEventPortMapping, TiledEventPortMapping,
                 SubgraphEventPortMapping>;

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
  NodeBundleHandle append_tiled(GraphBuilderTopology const &,
                                std::span<size_t const> concrete_node_indices,
                                ChannelLayout promoted_channel_layout);
  NodeBundleHandle append_subgraph(GraphBuilderTopology const &,
                                   size_t subgraph_node_index);
  NodeBundle const &bundle(NodeBundleHandle) const;
  NodeBundle &bundle(NodeBundleHandle);
  TiledSamplePortMapping const &tiled_sample_output(
      NodeBundleHandle, size_t output_ordinal) const;
  TiledSamplePortMapping const &tiled_sample_input(
      NodeBundleHandle, size_t input_ordinal) const;
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
