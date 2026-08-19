#pragma once

#include <intravenous/graph/builder/stored_node.h>
#include <intravenous/graph/builder/topology_port.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <span>
#include <variant>
#include <vector>

namespace iv {
using NodeBundleHandle = size_t;

struct NodeBundlePortId {
  NodeBundleHandle node_bundle_handle = 0;
  PortKind port_kind = PortKind::sample;
  size_t port_ordinal = 0;
  bool operator==(NodeBundlePortId const &) const = default;
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

// Closed builder-time representation for one node insertion result. Bundle-kind
// dispatch stays inside this subsystem so callers only depend on NodeBundle's
// logical port and node-membership operations.
class NodeBundle {
  struct ConcreteNodeBundle {
    static constexpr bool contains_concrete_nodes = true;

    size_t node{};
    std::vector<TopologyPortId> sample_inputs, sample_outputs{};
    std::vector<TopologyPortId> event_inputs, event_outputs{};
    std::vector<InputConfig> sample_input_configs{};
    std::vector<OutputConfig> sample_output_configs{};
    std::vector<EventInputConfig> event_input_configs{};
    std::vector<EventOutputConfig> event_output_configs{};
  };

  struct TiledNodeBundle {
    static constexpr bool contains_concrete_nodes = true;

    std::vector<size_t> nodes{};
    std::vector<std::vector<TopologyPortId>> sample_inputs, sample_outputs{};
    std::vector<std::vector<TopologyPortId>> event_inputs, event_outputs{};
    std::vector<InputConfig> sample_input_configs{};
    std::vector<OutputConfig> sample_output_configs{};
    std::vector<EventInputConfig> event_input_configs{};
    std::vector<EventOutputConfig> event_output_configs{};
  };

  struct SubgraphNodeBundle {
    static constexpr bool contains_concrete_nodes = false;

    size_t node{};
    std::vector<TopologyPortId> sample_inputs, sample_outputs{};
    std::vector<TopologyPortId> event_inputs, event_outputs{};
    std::vector<InputConfig> sample_input_configs{};
    std::vector<OutputConfig> sample_output_configs{};
    std::vector<EventInputConfig> event_input_configs{};
    std::vector<EventOutputConfig> event_output_configs{};
  };

  using Payload = std::variant<ConcreteNodeBundle, TiledNodeBundle,
                               SubgraphNodeBundle>;

public:
  NodeBundle() = default;
  NodeBundle(NodeBundle const &) = default;
  NodeBundle(NodeBundle &&) noexcept = default;
  NodeBundle &operator=(NodeBundle const &) = default;
  NodeBundle &operator=(NodeBundle &&) noexcept = default;
  ~NodeBundle() = default;

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
  explicit NodeBundle(ConcreteNodeBundle);
  explicit NodeBundle(TiledNodeBundle);
  explicit NodeBundle(SubgraphNodeBundle);

  std::optional<Payload> _payload{};
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
