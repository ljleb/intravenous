#pragma once

#include <intravenous/graph/builder/stored_node.h>

#include <cstddef>
#include <functional>
#include <string_view>
#include <span>
#include <vector>

namespace iv {
using NodeBundleHandle = size_t;

struct NodeBundlePortId {
  NodeBundleHandle node_bundle_handle = 0;
  PortKind port_kind = PortKind::sample;
  size_t port_ordinal = 0;
  bool operator==(NodeBundlePortId const &) const = default;
};

// Transitional name for a port in GraphBuilderTopology.  The topology still
// stores these addresses in ConcretePortId today, even when the node is a
// SubgraphNode.  Keep that legacy representation local to the bundle/lowering
// boundary instead of teaching callers to infer a node kind from the ID.
using TopologyPortId = ConcretePortId;

using ConcreteNodeId = size_t;
using SubgraphNodeId = size_t;

class GraphBuilderTopology;

// Manual type erasure for one node insertion result. Its private payload is a
// concrete, tiled, or subgraph bundle. There is no kind tag: each operation
// lowers directly through the payload that implements it.
class NodeBundle {
public:
  struct Ops;
  NodeBundle();
  NodeBundle(NodeBundle const &);
  NodeBundle(NodeBundle &&) noexcept;
  NodeBundle &operator=(NodeBundle const &);
  NodeBundle &operator=(NodeBundle &&) noexcept;
  ~NodeBundle();

  // Used only by the private payload factories in node_bundles.cpp. Ops and
  // payload types are not exposed to callers.
  NodeBundle(void *, Ops const *);

  void for_each_sample_input(size_t, std::function<void(TopologyPortId)> const &) const;
  void for_each_sample_output(size_t, std::function<void(TopologyPortId)> const &) const;
  void for_each_event_input(size_t, std::function<void(TopologyPortId)> const &) const;
  void for_each_event_output(size_t, std::function<void(TopologyPortId)> const &) const;
  ChannelLayout sample_input_layout(size_t) const;
  ChannelLayout sample_output_layout(size_t) const;
  InputConfig sample_input_config(GraphBuilderTopology const &, size_t) const;
  OutputConfig sample_output_config(GraphBuilderTopology const &, size_t) const;
  EventInputConfig event_input_config(GraphBuilderTopology const &, size_t) const;
  EventOutputConfig event_output_config(GraphBuilderTopology const &, size_t) const;
  std::string_view type_identity(GraphBuilderTopology const &) const;
  size_t sample_input_count() const;
  size_t sample_output_count() const;
  size_t event_input_count() const;
  size_t event_output_count() const;
  size_t sample_input_index(std::string_view) const;
  size_t sample_output_index(std::string_view) const;
  size_t event_input_index(std::string_view) const;
  size_t event_output_index(std::string_view) const;
  void for_each_topology_node(std::function<void(size_t)> const &) const;
  void for_each_concrete_node(std::function<void(size_t)> const &) const;
  size_t single_concrete_node() const;
  void import_into(size_t topology_node_offset);

  std::vector<size_t> &virtual_node_handles();
  std::vector<size_t> const &virtual_node_handles() const;
  NodeSourceAnnotations &source_annotations();
  NodeSourceAnnotations const &source_annotations() const;

private:
  void *_payload = nullptr;
  Ops const *_ops = nullptr;
  std::vector<size_t> _virtual_node_handles{};
  NodeSourceAnnotations _source_annotations{};
  friend class GraphBuilderNodeBundles;
};

class GraphBuilderNodeBundles {
public:
  NodeBundleHandle append_concrete(GraphBuilderTopology const &, size_t concrete_node_index);
  NodeBundleHandle append_tiled(GraphBuilderTopology const &, std::span<size_t const>,
                                ChannelLayout promoted_channel_layout);
  NodeBundleHandle append_subgraph(GraphBuilderTopology const &, size_t subgraph_node_index);
  NodeBundle const &bundle(NodeBundleHandle) const;
  NodeBundle &bundle(NodeBundleHandle);
  NodeBundleHandle bundle_for_concrete_node(size_t concrete_node_index) const;
  size_t size() const;
  void import_child(GraphBuilderNodeBundles const &, size_t concrete_node_offset);

private:
  std::vector<NodeBundle> _bundles{};
  std::vector<size_t> _bundle_by_concrete_node{};
};
} // namespace iv
