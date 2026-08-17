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

// A port on a node stored in GraphBuilderTopology. The node may be either a
// ConcreteNode or a SubgraphNode; the address itself does not imply a node
// kind. Keep this distinct from ConcretePortId, which is the execution-graph
// port type after lowering.
//
// The conversion is a temporary migration bridge for builder services that
// still store topology edges in ConcretePortId. Remove it once those services
// consume TopologyPortId directly.
struct TopologyPortId {
  size_t node = 0;
  size_t port = 0;

  constexpr TopologyPortId() = default;
  constexpr TopologyPortId(size_t node_, size_t port_) : node(node_), port(port_) {}

  operator ConcretePortId() const noexcept { return {node, port}; }

  bool operator==(TopologyPortId const &) const = default;
};

template<class Config>
struct SamplePortDescriptor {
  Config config{};
  std::vector<TopologyPortId> endpoints{};
};

template<class Config>
struct EventPortDescriptor {
  Config config{};
  std::vector<TopologyPortId> endpoints{};
};

using SampleInputPortDescriptor = SamplePortDescriptor<InputConfig>;
using SampleOutputPortDescriptor = SamplePortDescriptor<OutputConfig>;
using EventInputPortDescriptor = EventPortDescriptor<EventInputConfig>;
using EventOutputPortDescriptor = EventPortDescriptor<EventOutputConfig>;

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

  SampleInputPortDescriptor sample_input_descriptor(size_t) const;
  SampleOutputPortDescriptor sample_output_descriptor(size_t) const;
  EventInputPortDescriptor event_input_descriptor(size_t) const;
  EventOutputPortDescriptor event_output_descriptor(size_t) const;

  // Compatibility projection APIs. Consumers should migrate to descriptors.
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

  // Central builder/lowering boundary for logical bundle ports. Callers should
  // resolve NodeBundlePortId here instead of reaching into a bundle payload.
  SampleInputPortDescriptor resolve_sample_input(NodeBundlePortId) const;
  SampleOutputPortDescriptor resolve_sample_output(NodeBundlePortId) const;
  EventInputPortDescriptor resolve_event_input(NodeBundlePortId) const;
  EventOutputPortDescriptor resolve_event_output(NodeBundlePortId) const;

  NodeBundleHandle bundle_for_concrete_node(size_t concrete_node_index) const;
  size_t size() const;
  void import_child(GraphBuilderNodeBundles const &, size_t concrete_node_offset);

private:
  std::vector<NodeBundle> _bundles{};
  std::vector<size_t> _bundle_by_concrete_node{};
};
} // namespace iv
