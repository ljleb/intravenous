#pragma once

#include <intravenous/basic_nodes/routing.h>
#include <intravenous/graph/builder/names.h>
#include <intravenous/graph/builder/stored_node.h>

#include <concepts>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
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

struct SampleOutputChannelId {
  NodeBundleHandle bundle = 0;
  size_t port = 0;
  size_t channel = 0;
  bool operator==(SampleOutputChannelId const &) const = default;
};

struct SampleInputChannelId {
  NodeBundleHandle bundle = 0;
  size_t port = 0;
  size_t channel = 0;
  bool operator==(SampleInputChannelId const &) const = default;
};

struct EventOutputPortId {
  NodeBundleHandle bundle = 0;
  size_t port = 0;
  bool operator==(EventOutputPortId const &) const = default;
};

struct EventInputPortId {
  NodeBundleHandle bundle = 0;
  size_t port = 0;
  bool operator==(EventInputPortId const &) const = default;
};

template<class Config>
struct SamplePortDescriptor {
  Config config{};
};

template<class Config>
struct EventPortDescriptor {
  Config config{};
};

using SampleInputPortDescriptor = SamplePortDescriptor<InputConfig>;
using SampleOutputPortDescriptor = SamplePortDescriptor<OutputConfig>;
using EventInputPortDescriptor = EventPortDescriptor<EventInputConfig>;
using EventOutputPortDescriptor = EventPortDescriptor<EventOutputConfig>;

class GraphBuilderLowering;

class NodeBundle {
  struct ConcreteNodeBundle {
    NodePorts ports{};
    NodeMaterialization materialization{};
    NodeLifetime lifetime{};
    NodeTypeIdentity type_identity{};
  };

  struct TiledNodeBundle {
    std::vector<NodeBundleHandle> member_bundles{};
    NodeTypeIdentity type_identity{};
    std::vector<InputConfig> sample_input_configs{};
    std::vector<OutputConfig> sample_output_configs{};
    std::vector<EventInputConfig> event_input_configs{};
    std::vector<EventOutputConfig> event_output_configs{};
  };

  struct BoundaryNodeBundle {
    std::vector<InputConfig> sample_inputs{};
    std::vector<OutputConfig> sample_outputs{};
    std::vector<EventInputConfig> event_inputs{};
    std::vector<EventOutputConfig> event_outputs{};
  };

  // A subgraph owns hierarchy/lifetime identity, but its interface is owned by
  // the referenced BoundaryNodeBundle. Counts are cached only so NodeRef can
  // answer shape queries without owning GraphBuilderNodeBundles; all configs
  // are resolved through the boundary by GraphBuilderNodeBundles.
  struct SubgraphNodeBundle {
    NodeBundleHandle boundary{};
    size_t child_begin = 0;
    size_t child_count = 0;
    std::string kind{};
    NodeLifetime lifetime{};
    NodeTypeIdentity type_identity{};
    size_t sample_input_count = 0;
    size_t sample_output_count = 0;
    size_t event_input_count = 0;
    size_t event_output_count = 0;
  };

  using Payload = std::variant<ConcreteNodeBundle, TiledNodeBundle,
                               BoundaryNodeBundle, SubgraphNodeBundle>;

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

  bool is_concrete() const;
  bool is_tiled() const;
  bool is_boundary() const;
  bool is_subgraph() const;
  std::optional<NodeBundleHandle> subgraph_boundary_handle() const;
  std::span<NodeBundleHandle const> tiled_members() const;
  size_t subgraph_child_begin() const;
  size_t subgraph_child_count() const;
  std::string_view subgraph_kind() const;

  std::span<InputConfig const> boundary_sample_inputs() const;
  std::span<OutputConfig const> boundary_sample_outputs() const;
  std::span<EventInputConfig const> boundary_event_inputs() const;
  std::span<EventOutputConfig const> boundary_event_outputs() const;
  size_t append_boundary_sample_input(InputConfig);
  size_t append_boundary_sample_output(OutputConfig);
  size_t append_boundary_event_input(EventInputConfig);
  size_t append_boundary_event_output(EventOutputConfig);
  void clear_boundary_event_outputs();

  ChannelLayout sample_input_layout(size_t) const;
  ChannelLayout sample_output_layout(size_t) const;
  InputConfig sample_input_config(size_t) const;
  OutputConfig sample_output_config(size_t) const;
  EventInputConfig event_input_config(size_t) const;
  EventOutputConfig event_output_config(size_t) const;
  std::string_view type_identity() const;
  NodePorts const &concrete_ports() const;
  size_t sample_input_count() const;
  size_t sample_output_count() const;
  size_t event_input_count() const;
  size_t event_output_count() const;
  size_t sample_input_index(std::string_view) const;
  size_t sample_output_index(std::string_view) const;
  size_t event_input_index(std::string_view) const;
  size_t event_output_index(std::string_view) const;
  void import_into(size_t node_bundle_offset, size_t detach_id_offset);

  std::vector<size_t> &virtual_node_handles();
  std::vector<size_t> const &virtual_node_handles() const;
  NodeSourceAnnotations &source_annotations();
  NodeSourceAnnotations const &source_annotations() const;

private:
  explicit NodeBundle(ConcreteNodeBundle);
  explicit NodeBundle(TiledNodeBundle);
  explicit NodeBundle(BoundaryNodeBundle);
  explicit NodeBundle(SubgraphNodeBundle);

  std::optional<Payload> _payload{};
  std::vector<size_t> _virtual_node_handles{};
  NodeSourceAnnotations _source_annotations{};

  friend class GraphBuilderNodeBundles;
  friend class GraphBuilderLowering;
};

struct SemanticSubgraphInfo {
  NodeBundleHandle boundary = 0;
  size_t child_begin = 0;
  size_t child_count = 0;
  std::string kind{};
  NodeLifetime lifetime{};
};

class GraphBuilderNodeBundles {
public:
  template <class Config>
  static void validate_output_port_configs(std::span<Config const> configs,
                                           std::string_view node_label,
                                           std::string_view kind);

  template <class Node, class... Args>
  static ConcreteNode make_concrete_node(Args &&...args);

  NodeBundleHandle append_boundary();
  NodeBundleHandle append_scope_boundary();

  template <class Node, class... Args>
  NodeBundleHandle append_concrete(Args &&...args);

  NodeBundleHandle append_tiled(
      std::span<NodeBundleHandle const>, ChannelLayout promoted_channel_layout);
  NodeBundleHandle append_subgraph(NodeBundleHandle boundary,
                                   size_t child_begin, size_t child_count,
                                   std::string_view kind);
  NodeBundle const &bundle(NodeBundleHandle) const;
  NodeBundle &bundle(NodeBundleHandle);

  SampleInputPortDescriptor resolve_sample_input(NodeBundlePortId) const;
  SampleOutputPortDescriptor resolve_sample_output(NodeBundlePortId) const;
  EventInputPortDescriptor resolve_event_input(NodeBundlePortId) const;
  EventOutputPortDescriptor resolve_event_output(NodeBundlePortId) const;
  std::vector<SampleInputChannelId> sample_input_channels(NodeBundlePortId) const;
  std::vector<SampleOutputChannelId> sample_output_channels(NodeBundlePortId) const;
  std::optional<NodeBundlePortId> sample_output_port_for_channels(
      ChannelTypeId, std::span<SampleOutputChannelId const>) const;
  std::vector<EventInputPortId> event_input_ports(NodeBundlePortId) const;
  std::vector<EventOutputPortId> event_output_ports(NodeBundlePortId) const;

  NodePorts const &typed_ports(NodeBundleHandle) const;
  NodeBundleHandle tiled_member(NodeBundleHandle, size_t channel) const;
  ConcreteNode lowered_concrete(NodeBundleHandle) const;
  SemanticSubgraphInfo subgraph_info(NodeBundleHandle) const;

  size_t size() const;
  void apply_ttl(NodeBundleHandle, size_t ttl_samples);
  size_t import_child(GraphBuilderNodeBundles const &, size_t detach_id_offset);

private:
  std::vector<NodeBundle> _bundles{};
};

template <class Config>
void GraphBuilderNodeBundles::validate_output_port_configs(
    std::span<Config const> configs, std::string_view node_label,
    std::string_view kind) {
  if (configs.size() <= 1) return;
  for (auto const &config : configs) {
    if (config.name.empty()) {
      details::error(std::string(node_label) + ": output " +
                     std::string(kind) +
                     " ports require names when more than one output is exposed");
    }
  }
}

template <class Node, class... Args>
ConcreteNode GraphBuilderNodeBundles::make_concrete_node(Args &&...args) {
  using StoredNode = std::remove_cvref_t<Node>;
  static_assert(
      details::has_constexpr_sample_port_configs<StoredNode>,
      "concrete DSP nodes must provide constexpr static inputs()/outputs() configurations");

  StoredNode node_value(std::forward<Args>(args)...);
  auto inputs = get_inputs(node_value);
  auto outputs = get_outputs(node_value);
  auto event_inputs = get_event_inputs(node_value);
  auto event_outputs = get_event_outputs(node_value);

  auto const node_type = typeid(StoredNode).name();
  validate_output_port_configs(
      std::span<OutputConfig const>(std::begin(outputs), std::end(outputs)),
      node_type, "sample");
  validate_output_port_configs(
      std::span<EventOutputConfig const>(std::begin(event_outputs),
                                         std::end(event_outputs)),
      node_type, "event");

  auto materialize =
      [node_value = std::move(node_value)](
          [[maybe_unused]] size_t detach_id_offset) {
        if constexpr (std::same_as<StoredNode, DetachWriterNode>) {
          return TypeErasedNode(DetachWriterNode{
              DetachArrayId(node_value.id.id + detach_id_offset),
              node_value.loop_extra_latency});
        } else if constexpr (std::same_as<StoredNode, DetachReaderNode>) {
          return TypeErasedNode(DetachReaderNode{
              DetachArrayId(node_value.id.id + detach_id_offset),
              node_value.loop_extra_latency});
        } else {
          return TypeErasedNode(node_value);
        }
      };

  return ConcreteNode{
      .ports = NodePorts{
          .sample_inputs =
              std::vector<InputConfig>(std::begin(inputs), std::end(inputs)),
          .sample_outputs =
              std::vector<OutputConfig>(std::begin(outputs), std::end(outputs)),
          .event_input_configs = std::vector<EventInputConfig>(
              std::begin(event_inputs), std::end(event_inputs)),
          .event_output_configs = std::vector<EventOutputConfig>(
              std::begin(event_outputs), std::end(event_outputs)),
      },
      .materialization = NodeMaterialization{.factory = std::move(materialize)},
      .type_identity = NodeTypeIdentity{
          .value = details::demangle_type_name(typeid(StoredNode).name())},
  };
}

template <class Node, class... Args>
NodeBundleHandle GraphBuilderNodeBundles::append_concrete(Args &&...args) {
  auto lowered = make_concrete_node<Node>(std::forward<Args>(args)...);
  NodeBundle::ConcreteNodeBundle payload{
      .ports = std::move(lowered.ports),
      .materialization = std::move(lowered.materialization),
      .lifetime = std::move(lowered.lifetime),
      .type_identity = std::move(lowered.type_identity),
  };
  auto const handle = _bundles.size();
  _bundles.push_back(NodeBundle(std::move(payload)));
  return handle;
}
} // namespace iv
