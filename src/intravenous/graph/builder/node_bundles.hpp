#pragma once

#include <intravenous/basic_nodes/routing.h>
#include <intravenous/graph/error.h>
#include <intravenous/graph/names.h>
#include <intravenous/graph/builder/stored_node.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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

struct NodeBundlePortIdLess {
  constexpr bool operator()(NodeBundlePortId const& lhs,
                            NodeBundlePortId const& rhs) const {
    if (lhs.node_bundle_handle != rhs.node_bundle_handle)
      return lhs.node_bundle_handle < rhs.node_bundle_handle;
    if (lhs.port_kind != rhs.port_kind)
      return lhs.port_kind < rhs.port_kind;
    return lhs.port_ordinal < rhs.port_ordinal;
  }
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

class NodeBundle {
  struct ConcreteNodeBundle {
    NodePorts ports{};
    ReflectedNodeOperations operations{};
    NodeLifetime lifetime{};
    NodeTypeIdentity type_identity{};
    std::string reflected_type_name{};
    size_t internal_latency_samples = 0;
    size_t maximum_block_size = MAX_BLOCK_SIZE;
    std::optional<size_t> default_ttl_samples{};
    bool block_skippable = false;
    std::optional<Sample> static_sample_value{};
    std::optional<DeferredDetachNode> deferred_detach{};
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
  constexpr NodeBundle() = default;
  constexpr NodeBundle(NodeBundle const &) = default;
  constexpr NodeBundle(NodeBundle &&) noexcept = default;
  constexpr NodeBundle &operator=(NodeBundle const &) = default;
  constexpr NodeBundle &operator=(NodeBundle &&) noexcept = default;
  constexpr ~NodeBundle() = default;

  constexpr SampleInputPortDescriptor sample_input_descriptor(size_t) const;
  constexpr SampleOutputPortDescriptor sample_output_descriptor(size_t) const;
  constexpr EventInputPortDescriptor event_input_descriptor(size_t) const;
  constexpr EventOutputPortDescriptor event_output_descriptor(size_t) const;

  constexpr bool is_concrete() const;
  constexpr bool is_tiled() const;
  constexpr bool is_boundary() const;
  constexpr bool is_subgraph() const;
  constexpr std::optional<NodeBundleHandle> subgraph_boundary_handle() const;
  constexpr std::span<NodeBundleHandle const> tiled_members() const;
  constexpr size_t subgraph_child_begin() const;
  constexpr size_t subgraph_child_count() const;
  constexpr std::string_view subgraph_kind() const;

  constexpr std::span<InputConfig const> boundary_sample_inputs() const;
  constexpr std::span<OutputConfig const> boundary_sample_outputs() const;
  constexpr std::span<EventInputConfig const> boundary_event_inputs() const;
  constexpr std::span<EventOutputConfig const> boundary_event_outputs() const;
  constexpr size_t append_boundary_sample_input(InputConfig);
  constexpr size_t append_boundary_sample_output(OutputConfig);
  constexpr size_t append_boundary_event_input(EventInputConfig);
  constexpr size_t append_boundary_event_output(EventOutputConfig);
  constexpr void clear_boundary_event_outputs();

  constexpr ChannelLayout sample_input_layout(size_t) const;
  constexpr ChannelLayout sample_output_layout(size_t) const;
  constexpr InputConfig sample_input_config(size_t) const;
  constexpr OutputConfig sample_output_config(size_t) const;
  constexpr EventInputConfig event_input_config(size_t) const;
  constexpr EventOutputConfig event_output_config(size_t) const;
  constexpr std::string_view type_identity() const;
  constexpr NodePorts const &concrete_ports() const;
  constexpr size_t sample_input_count() const;
  constexpr size_t sample_output_count() const;
  constexpr size_t event_input_count() const;
  constexpr size_t event_output_count() const;
  constexpr size_t sample_input_index(std::string_view) const;
  constexpr size_t sample_output_index(std::string_view) const;
  constexpr size_t event_input_index(std::string_view) const;
  constexpr size_t event_output_index(std::string_view) const;
  constexpr void import_into(
      size_t node_bundle_offset, size_t detach_id_offset);

  constexpr std::vector<size_t> &virtual_node_handles();
  constexpr std::vector<size_t> const &virtual_node_handles() const;
  constexpr NodeSourceAnnotations &source_annotations();
  constexpr NodeSourceAnnotations const &source_annotations() const;

private:
  constexpr explicit NodeBundle(ConcreteNodeBundle payload)
      : _payload(Payload{std::move(payload)}) {}
  constexpr explicit NodeBundle(TiledNodeBundle payload)
      : _payload(Payload{std::move(payload)}) {}
  constexpr explicit NodeBundle(BoundaryNodeBundle payload)
      : _payload(Payload{std::move(payload)}) {}
  constexpr explicit NodeBundle(SubgraphNodeBundle payload)
      : _payload(Payload{std::move(payload)}) {}

  std::optional<Payload> _payload{};
  std::vector<size_t> _virtual_node_handles{};
  NodeSourceAnnotations _source_annotations{};

  friend class GraphBuilderNodeBundles;
};

struct SemanticSubgraphInfo {
  NodeBundleHandle boundary = 0;
  size_t child_begin = 0;
  size_t child_count = 0;
  std::string kind{};
  NodeLifetime lifetime{};
};

enum class AuthoredNodeBundleKind : std::uint8_t {
  concrete,
  tiled,
  boundary,
  subgraph,
};

// A public, lossless record of the semantic bundle.  It deliberately contains
// no NodeBundle implementation details, so the frozen module ABI can be
// defined in terms of this data rather than the variant used by GraphBuilder.
struct AuthoredNodeBundleRecord {
  AuthoredNodeBundleKind kind = AuthoredNodeBundleKind::boundary;
  NodePorts ports{};
  ReflectedNodeOperations operations{};
  NodeLifetime lifetime{};
  std::string type_identity{};
  std::string reflected_type_name{};
  size_t internal_latency_samples = 0;
  size_t maximum_block_size = MAX_BLOCK_SIZE;
  std::optional<size_t> default_ttl_samples{};
  bool block_skippable = false;
  std::optional<Sample> static_sample_value{};
  std::optional<DeferredDetachNode> deferred_detach{};

  std::vector<NodeBundleHandle> tiled_members{};
  std::vector<InputConfig> sample_input_configs{};
  std::vector<OutputConfig> sample_output_configs{};
  std::vector<EventInputConfig> event_input_configs{};
  std::vector<EventOutputConfig> event_output_configs{};

  NodeBundleHandle subgraph_boundary = 0;
  size_t subgraph_child_begin = 0;
  size_t subgraph_child_count = 0;
  std::string subgraph_kind{};
  size_t subgraph_sample_input_count = 0;
  size_t subgraph_sample_output_count = 0;
  size_t subgraph_event_input_count = 0;
  size_t subgraph_event_output_count = 0;

  std::vector<size_t> virtual_node_handles{};
  std::vector<SourceInfo> source_infos{};
};

// A non-owning serialization view of a semantic bundle. It is valid only for
// the duration of the for_each_authored_bundle callback, allowing freezing to
// promote GraphBuilder storage directly without materializing an owning record.
struct AuthoredNodeBundleView {
  AuthoredNodeBundleKind kind = AuthoredNodeBundleKind::boundary;
  NodePorts const* ports = nullptr;
  ReflectedNodeOperations const* operations = nullptr;
  NodeLifetime const* lifetime = nullptr;
  std::string const* type_identity = nullptr;
  std::string const* reflected_type_name = nullptr;
  size_t internal_latency_samples = 0;
  size_t maximum_block_size = MAX_BLOCK_SIZE;
  std::optional<size_t> const* default_ttl_samples = nullptr;
  bool block_skippable = false;
  std::optional<Sample> const* static_sample_value = nullptr;
  std::optional<DeferredDetachNode> const* deferred_detach = nullptr;

  std::span<NodeBundleHandle const> tiled_members{};
  std::span<InputConfig const> sample_input_configs{};
  std::span<OutputConfig const> sample_output_configs{};
  std::span<EventInputConfig const> event_input_configs{};
  std::span<EventOutputConfig const> event_output_configs{};

  NodeBundleHandle subgraph_boundary = 0;
  size_t subgraph_child_begin = 0;
  size_t subgraph_child_count = 0;
  std::string const* subgraph_kind = nullptr;
  size_t subgraph_sample_input_count = 0;
  size_t subgraph_sample_output_count = 0;
  size_t subgraph_event_input_count = 0;
  size_t subgraph_event_output_count = 0;

  std::span<size_t const> virtual_node_handles{};
  std::span<SourceInfo const> source_infos{};
};

class GraphBuilderNodeBundles {
public:
  template <class Config>
  static constexpr void validate_output_port_configs(
      std::span<Config const> configs,
      std::string_view node_label,
      std::string_view kind);

  static constexpr ConcreteNode make_concrete_node(
      ReflectedNodeDescription description);

  template<class Node, class... Args>
  static constexpr ConcreteNode make_concrete_node(Args&&... args);

  constexpr NodeBundleHandle append_boundary();
  constexpr NodeBundleHandle append_scope_boundary();

  constexpr NodeBundleHandle append_concrete(ConcreteNode node);
  constexpr NodeBundleHandle append_deferred_detach_writer(
      size_t detach_id, size_t loop_extra_latency);
  constexpr NodeBundleHandle append_deferred_detach_reader(
      size_t detach_id, size_t loop_extra_latency);
  constexpr void materialize_deferred_detaches();

  constexpr NodeBundleHandle append_tiled(
      std::span<NodeBundleHandle const>, ChannelLayout promoted_channel_layout);
  constexpr NodeBundleHandle append_subgraph(
      NodeBundleHandle boundary, size_t child_begin, size_t child_count,
      std::string_view kind);
  constexpr NodeBundle const &bundle(NodeBundleHandle) const;
  constexpr NodeBundle &bundle(NodeBundleHandle);

  constexpr SampleInputPortDescriptor resolve_sample_input(
      NodeBundlePortId) const;
  constexpr SampleOutputPortDescriptor resolve_sample_output(
      NodeBundlePortId) const;
  constexpr EventInputPortDescriptor resolve_event_input(
      NodeBundlePortId) const;
  constexpr EventOutputPortDescriptor resolve_event_output(
      NodeBundlePortId) const;
  constexpr std::vector<SampleInputChannelId> sample_input_channels(
      NodeBundlePortId) const;
  constexpr std::vector<SampleOutputChannelId> sample_output_channels(
      NodeBundlePortId) const;
  constexpr std::optional<NodeBundlePortId> sample_output_port_for_channels(
      ChannelTypeId, std::span<SampleOutputChannelId const>) const;
  constexpr std::vector<EventInputPortId> event_input_ports(
      NodeBundlePortId) const;
  constexpr std::vector<EventOutputPortId> event_output_ports(
      NodeBundlePortId) const;

  constexpr NodePorts const &typed_ports(NodeBundleHandle) const;
  constexpr NodeBundleHandle tiled_member(
      NodeBundleHandle, size_t channel) const;
  constexpr NodeLifetime const &concrete_lifetime(NodeBundleHandle) const;
  constexpr ReflectedNodeDescription materialize_concrete_description(
      NodeBundleHandle) const;
  constexpr SemanticSubgraphInfo subgraph_info(NodeBundleHandle) const;

  constexpr size_t size() const { return _bundles.size(); }
  constexpr void apply_ttl(NodeBundleHandle, size_t ttl_samples);
  constexpr size_t import_child(
      GraphBuilderNodeBundles const &, size_t detach_id_offset);
  template<class Visitor>
  constexpr void for_each_authored_bundle(Visitor&& visitor) const;
  static constexpr GraphBuilderNodeBundles from_authored_records(
      std::span<AuthoredNodeBundleRecord const>);

private:
  std::vector<NodeBundle> _bundles{};
};

template <class Config>
constexpr void GraphBuilderNodeBundles::validate_output_port_configs(
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

constexpr ConcreteNode GraphBuilderNodeBundles::make_concrete_node(
    ReflectedNodeDescription description) {
  validate_output_port_configs(
      std::span<OutputConfig const>(description.ports.outputs()),
      description.type_name,
      "sample");
  validate_output_port_configs(
      std::span<EventOutputConfig const>(description.ports.event_outputs()),
      description.type_name,
      "event");

  return ConcreteNode{
      .ports = std::move(description.ports),
      .operations = description.operations,
      .type_identity = NodeTypeIdentity{.value = std::string(description.type_name)},
      .reflected_type_name = description.type_name,
      .internal_latency_samples = description.internal_latency_samples,
      .maximum_block_size = description.maximum_block_size,
      .default_ttl_samples = description.default_ttl_samples,
      .block_skippable = description.block_skippable,
      .static_sample_value = description.static_sample_value,
  };
}

template<class Node, class... Args>
constexpr ConcreteNode GraphBuilderNodeBundles::make_concrete_node(
    Args&&... args) {
  if consteval {
    Node node(std::forward<Args>(args)...);
    return make_concrete_node(details::reflect_node(node));
  } else {
    details::runtime_graph_builder_node_call_is_forbidden();
    return {};
  }
}

constexpr NodeBundleHandle GraphBuilderNodeBundles::append_concrete(
    ConcreteNode lowered) {
  NodeBundle::ConcreteNodeBundle payload{
      .ports = std::move(lowered.ports),
      .operations = lowered.operations,
      .lifetime = std::move(lowered.lifetime),
      .type_identity = std::move(lowered.type_identity),
      .reflected_type_name = std::string(lowered.reflected_type_name),
      .internal_latency_samples = lowered.internal_latency_samples,
      .maximum_block_size = lowered.maximum_block_size,
      .default_ttl_samples = lowered.default_ttl_samples,
      .block_skippable = lowered.block_skippable,
      .static_sample_value = lowered.static_sample_value,
      .deferred_detach = lowered.deferred_detach,
  };
  auto const handle = _bundles.size();
  _bundles.push_back(NodeBundle(std::move(payload)));
  return handle;
}

constexpr NodeBundleHandle
GraphBuilderNodeBundles::append_deferred_detach_writer(
    size_t detach_id, size_t loop_extra_latency) {
  ConcreteNode node;
  node.ports.sample_inputs = {InputConfig{}};
  node.type_identity = {.value = std::string(
      details::reflected_node_type_metadata<DetachWriterNode>.type_name)};
  node.deferred_detach = DeferredDetachNode{
      .kind = DeferredDetachNodeKind::writer,
      .id = detach_id,
      .loop_extra_latency = loop_extra_latency,
  };
  return append_concrete(std::move(node));
}

constexpr NodeBundleHandle
GraphBuilderNodeBundles::append_deferred_detach_reader(
    size_t detach_id, size_t loop_extra_latency) {
  ConcreteNode node;
  node.ports.sample_outputs = {OutputConfig{}};
  node.type_identity = {.value = std::string(
      details::reflected_node_type_metadata<DetachReaderNode>.type_name)};
  node.deferred_detach = DeferredDetachNode{
      .kind = DeferredDetachNodeKind::reader,
      .id = detach_id,
      .loop_extra_latency = loop_extra_latency,
  };
  return append_concrete(std::move(node));
}

constexpr void GraphBuilderNodeBundles::materialize_deferred_detaches() {
  for (auto& bundle : _bundles) {
    if (!bundle.is_concrete()) continue;
    auto& payload = std::get<NodeBundle::ConcreteNodeBundle>(
        *bundle._payload);
    if (!payload.deferred_detach) continue;

    ConcreteNode materialized;
    auto const deferred = *payload.deferred_detach;
    if consteval {
      if (deferred.kind == DeferredDetachNodeKind::writer) {
        materialized = make_concrete_node(details::reflect_node(
            DetachWriterNode{
                .id = DetachArrayId{deferred.id},
                .loop_extra_latency = deferred.loop_extra_latency,
            }));
      } else {
        materialized = make_concrete_node(details::reflect_node(
            DetachReaderNode{
                .id = DetachArrayId{deferred.id},
                .loop_extra_latency = deferred.loop_extra_latency,
            }));
      }
    } else {
      details::runtime_graph_builder_node_call_is_forbidden();
      return;
    }

    payload.ports = std::move(materialized.ports);
    payload.operations = materialized.operations;
    payload.lifetime = std::move(materialized.lifetime);
    payload.type_identity = std::move(materialized.type_identity);
    payload.reflected_type_name = materialized.reflected_type_name;
    payload.internal_latency_samples = materialized.internal_latency_samples;
    payload.maximum_block_size = materialized.maximum_block_size;
    payload.default_ttl_samples = materialized.default_ttl_samples;
    payload.block_skippable = materialized.block_skippable;
    payload.static_sample_value = materialized.static_sample_value;
    payload.deferred_detach.reset();
  }
}

constexpr NodeBundleHandle GraphBuilderNodeBundles::append_boundary() {
  auto const handle = _bundles.size();
  _bundles.push_back(NodeBundle(NodeBundle::BoundaryNodeBundle{}));
  return handle;
}

constexpr NodeBundleHandle GraphBuilderNodeBundles::append_scope_boundary() {
  return append_boundary();
}

constexpr bool NodeBundle::is_concrete() const {
  return _payload && std::holds_alternative<ConcreteNodeBundle>(*_payload);
}

constexpr bool NodeBundle::is_tiled() const {
  return _payload && std::holds_alternative<TiledNodeBundle>(*_payload);
}

constexpr size_t NodeBundle::sample_output_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit([](auto const &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
      return payload.sample_inputs.size();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>)
      return payload.ports.sample_outputs.size();
    else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>)
      return payload.sample_output_configs.size();
    else
      return payload.sample_output_count;
  }, *_payload);
}

constexpr std::span<NodeBundleHandle const> NodeBundle::tiled_members() const {
  if (!_payload) details::error("empty NodeBundle");
  auto const *tiled = std::get_if<TiledNodeBundle>(&*_payload);
  if (!tiled) details::error("NodeBundle is not tiled");
  return tiled->member_bundles;
}

constexpr NodePorts const &NodeBundle::concrete_ports() const {
  auto const *concrete = _payload
      ? std::get_if<ConcreteNodeBundle>(&*_payload)
      : nullptr;
  if (!concrete)
    details::error("NodeBundle is not a concrete node bundle");
  return concrete->ports;
}

constexpr NodeBundle const &GraphBuilderNodeBundles::bundle(
    NodeBundleHandle handle) const {
  if (handle >= _bundles.size())
    details::error("NodeBundle handle is out of bounds");
  return _bundles[handle];
}

constexpr NodeBundle &GraphBuilderNodeBundles::bundle(
    NodeBundleHandle handle) {
  if (handle >= _bundles.size())
    details::error("NodeBundle handle is out of bounds");
  return _bundles[handle];
}

constexpr NodePorts const &GraphBuilderNodeBundles::typed_ports(
    NodeBundleHandle handle) const {
  auto const &candidate = bundle(handle);
  if (candidate.is_concrete()) return candidate.concrete_ports();
  if (candidate.is_tiled()) {
    auto const members = candidate.tiled_members();
    if (members.empty()) details::error("tiled NodeBundle has no members");
    return bundle(members.front()).concrete_ports();
  }
  details::error(
      "typed NodeRef does not refer to a concrete or tiled bundle");
}

constexpr std::optional<NodeBundleHandle>
NodeBundle::subgraph_boundary_handle() const {
  if (!_payload) details::error("empty NodeBundle");
  auto const *subgraph = std::get_if<SubgraphNodeBundle>(&*_payload);
  return subgraph
      ? std::optional<NodeBundleHandle>(subgraph->boundary)
      : std::nullopt;
}

constexpr std::span<OutputConfig const>
NodeBundle::boundary_sample_outputs() const {
  auto const *boundary = _payload
      ? std::get_if<BoundaryNodeBundle>(&*_payload)
      : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->sample_outputs;
}

constexpr size_t NodeBundle::append_boundary_sample_output(
    OutputConfig config) {
  auto *boundary = _payload
      ? std::get_if<BoundaryNodeBundle>(&*_payload)
      : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->sample_outputs.size();
  boundary->sample_outputs.push_back(std::move(config));
  return ordinal;
}

constexpr std::span<InputConfig const>
NodeBundle::boundary_sample_inputs() const {
  auto const *boundary = _payload
      ? std::get_if<BoundaryNodeBundle>(&*_payload)
      : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->sample_inputs;
}

constexpr SampleInputPortDescriptor
NodeBundle::sample_input_descriptor(size_t ordinal) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) -> SampleInputPortDescriptor {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) {
          if (ordinal >= payload.sample_outputs.size())
            details::error("NodeBundle port ordinal is out of bounds");
          auto const &output = payload.sample_outputs[ordinal];
          return {.config = InputConfig{
              .name = output.name,
              .channel_layout = output.channel_layout,
              .history = output.history,
          }};
        } else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
          if (ordinal >= payload.ports.sample_inputs.size())
            details::error("NodeBundle port ordinal is out of bounds");
          return {.config = payload.ports.sample_inputs[ordinal]};
        } else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
          if (ordinal >= payload.sample_input_configs.size())
            details::error("NodeBundle port ordinal is out of bounds");
          return {.config = payload.sample_input_configs[ordinal]};
        } else {
          details::error(
              "SubgraphNodeBundle port configs must be resolved through its boundary");
        }
      },
      *_payload);
}

constexpr SampleOutputPortDescriptor
NodeBundle::sample_output_descriptor(size_t ordinal) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) -> SampleOutputPortDescriptor {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) {
          if (ordinal >= payload.sample_inputs.size())
            details::error("NodeBundle port ordinal is out of bounds");
          auto const &input = payload.sample_inputs[ordinal];
          return {.config = OutputConfig{
              .name = input.name,
              .channel_layout = input.channel_layout,
              .history = input.history,
          }};
        } else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
          if (ordinal >= payload.ports.sample_outputs.size())
            details::error("NodeBundle port ordinal is out of bounds");
          return {.config = payload.ports.sample_outputs[ordinal]};
        } else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
          if (ordinal >= payload.sample_output_configs.size())
            details::error("NodeBundle port ordinal is out of bounds");
          return {.config = payload.sample_output_configs[ordinal]};
        } else {
          details::error(
              "SubgraphNodeBundle port configs must be resolved through its boundary");
        }
      },
      *_payload);
}

constexpr SampleOutputPortDescriptor
GraphBuilderNodeBundles::resolve_sample_output(NodeBundlePortId id) const {
  if (id.port_kind != PortKind::sample)
    details::error("sample output address has event kind");
  auto const &candidate = bundle(id.node_bundle_handle);
  if (auto boundary = candidate.subgraph_boundary_handle()) {
    auto const configs = bundle(*boundary).boundary_sample_outputs();
    if (id.port_ordinal >= configs.size())
      details::error("NodeBundle port ordinal is out of bounds");
    return {.config = configs[id.port_ordinal]};
  }
  return candidate.sample_output_descriptor(id.port_ordinal);
}

constexpr SampleInputPortDescriptor
GraphBuilderNodeBundles::resolve_sample_input(NodeBundlePortId id) const {
  if (id.port_kind != PortKind::sample)
    details::error("sample input address has event kind");
  auto const &candidate = bundle(id.node_bundle_handle);
  if (auto boundary = candidate.subgraph_boundary_handle()) {
    auto const configs = bundle(*boundary).boundary_sample_inputs();
    if (id.port_ordinal >= configs.size())
      details::error("NodeBundle port ordinal is out of bounds");
    return {.config = configs[id.port_ordinal]};
  }
  return candidate.sample_input_descriptor(id.port_ordinal);
}

constexpr std::vector<SampleOutputChannelId>
GraphBuilderNodeBundles::sample_output_channels(NodeBundlePortId id) const {
  auto const type =
      resolve_sample_output(id).config.channel_layout.channel_type;
  std::vector<SampleOutputChannelId> result;
  for (size_t channel = 0; channel < channel_count(type); ++channel) {
    result.push_back({
        id.node_bundle_handle,
        id.port_ordinal,
        channel,
    });
  }
  return result;
}

constexpr std::vector<SampleInputChannelId>
GraphBuilderNodeBundles::sample_input_channels(NodeBundlePortId id) const {
  auto const type = resolve_sample_input(id).config.channel_layout.channel_type;
  std::vector<SampleInputChannelId> result;
  for (size_t channel = 0; channel < channel_count(type); ++channel) {
    result.push_back({
        id.node_bundle_handle,
        id.port_ordinal,
        channel,
    });
  }
  return result;
}

constexpr std::optional<NodeBundlePortId>
GraphBuilderNodeBundles::sample_output_port_for_channels(
    ChannelTypeId type,
    std::span<SampleOutputChannelId const> channels) const {
  // A native output's channel IDs already carry its logical identity.  Do not
  // rediscover that identity by scanning every bundle and output port: validate
  // that this is the canonical consecutive channel sequence for its first ID.
  if (channels.empty()) return std::nullopt;
  auto const first = channels.front();
  if (first.bundle >= _bundles.size()) return std::nullopt;
  auto const& candidate = bundle(first.bundle);
  if (first.port >= candidate.sample_output_count()) return std::nullopt;

  NodeBundlePortId const id {first.bundle, PortKind::sample, first.port};
  if (resolve_sample_output(id).config.channel_layout.channel_type != type
      || channels.size() != channel_count(type))
    return std::nullopt;

  for (size_t channel = 0; channel < channels.size(); ++channel) {
    if (channels[channel]
        != SampleOutputChannelId{first.bundle, first.port, channel})
      return std::nullopt;
  }
  return id;
}
} // namespace iv
namespace iv {
namespace {
constexpr EventOutputConfig inward_event_output_config(
    EventInputConfig const &config) {
  return EventOutputConfig{.name = config.name, .type = config.type};
}

constexpr EventInputConfig inward_event_input_config(
    EventOutputConfig const &config) {
  return EventInputConfig{.name = config.name, .type = config.type};
}

template <class MatchesName>
constexpr size_t index_for_name(
    size_t count, MatchesName matches_name, std::string_view name) {
  std::optional<size_t> result;
  for (size_t ordinal = 0; ordinal < count; ++ordinal) {
    if (!matches_name(ordinal)) continue;
    if (result) {
      details::error("NodeBundle port name '" + std::string(name) +
                     "' is ambiguous");
    }
    result = ordinal;
  }
  if (!result) {
    details::error("NodeBundle port name '" + std::string(name) +
                   "' does not exist");
  }
  return *result;
}

template <class Descriptor, class Configs>
constexpr Descriptor descriptor(Configs const &configs, size_t ordinal) {
  if (ordinal >= configs.size()) {
    details::error("NodeBundle port ordinal is out of bounds");
  }
  return Descriptor{.config = configs[ordinal]};
}
} // namespace

constexpr EventInputPortDescriptor
NodeBundle::event_input_descriptor(size_t i) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) -> EventInputPortDescriptor {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) {
          if (i >= payload.event_outputs.size())
            details::error("NodeBundle port ordinal is out of bounds");
          return {.config = inward_event_input_config(payload.event_outputs[i])};
        } else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
          return descriptor<EventInputPortDescriptor>(
              payload.ports.event_input_configs, i);
        } else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
          return descriptor<EventInputPortDescriptor>(
              payload.event_input_configs, i);
        } else {
          details::error(
              "SubgraphNodeBundle port configs must be resolved through its boundary");
        }
      },
      *_payload);
}

constexpr EventOutputPortDescriptor
NodeBundle::event_output_descriptor(size_t i) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) -> EventOutputPortDescriptor {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) {
          if (i >= payload.event_inputs.size())
            details::error("NodeBundle port ordinal is out of bounds");
          return {.config = inward_event_output_config(payload.event_inputs[i])};
        } else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
          return descriptor<EventOutputPortDescriptor>(
              payload.ports.event_output_configs, i);
        } else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
          return descriptor<EventOutputPortDescriptor>(
              payload.event_output_configs, i);
        } else {
          details::error(
              "SubgraphNodeBundle port configs must be resolved through its boundary");
        }
      },
      *_payload);
}

constexpr bool NodeBundle::is_boundary() const {
  return _payload && std::holds_alternative<BoundaryNodeBundle>(*_payload);
}
constexpr bool NodeBundle::is_subgraph() const {
  return _payload && std::holds_alternative<SubgraphNodeBundle>(*_payload);
}

constexpr size_t NodeBundle::subgraph_child_begin() const {
  auto const *subgraph = _payload ? std::get_if<SubgraphNodeBundle>(&*_payload) : nullptr;
  if (!subgraph) details::error("NodeBundle is not a subgraph");
  return subgraph->child_begin;
}
constexpr size_t NodeBundle::subgraph_child_count() const {
  auto const *subgraph = _payload ? std::get_if<SubgraphNodeBundle>(&*_payload) : nullptr;
  if (!subgraph) details::error("NodeBundle is not a subgraph");
  return subgraph->child_count;
}
constexpr std::string_view NodeBundle::subgraph_kind() const {
  auto const *subgraph = _payload ? std::get_if<SubgraphNodeBundle>(&*_payload) : nullptr;
  if (!subgraph) details::error("NodeBundle is not a subgraph");
  return subgraph->kind;
}

constexpr std::span<EventInputConfig const>
NodeBundle::boundary_event_inputs() const {
  auto const *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->event_inputs;
}
constexpr std::span<EventOutputConfig const>
NodeBundle::boundary_event_outputs() const {
  auto const *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->event_outputs;
}

constexpr size_t NodeBundle::append_boundary_sample_input(InputConfig config) {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->sample_inputs.size();
  boundary->sample_inputs.push_back(std::move(config));
  return ordinal;
}
constexpr size_t NodeBundle::append_boundary_event_input(
    EventInputConfig config) {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->event_inputs.size();
  boundary->event_inputs.push_back(std::move(config));
  return ordinal;
}
constexpr size_t NodeBundle::append_boundary_event_output(
    EventOutputConfig config) {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->event_outputs.size();
  boundary->event_outputs.push_back(std::move(config));
  return ordinal;
}
constexpr void NodeBundle::clear_boundary_event_outputs() {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  boundary->event_outputs.clear();
}

constexpr ChannelLayout NodeBundle::sample_input_layout(size_t i) const {
  return sample_input_descriptor(i).config.channel_layout;
}
constexpr ChannelLayout NodeBundle::sample_output_layout(size_t i) const {
  return sample_output_descriptor(i).config.channel_layout;
}
constexpr InputConfig NodeBundle::sample_input_config(size_t i) const {
  return sample_input_descriptor(i).config;
}
constexpr OutputConfig NodeBundle::sample_output_config(size_t i) const {
  return sample_output_descriptor(i).config;
}
constexpr EventInputConfig NodeBundle::event_input_config(size_t i) const {
  return event_input_descriptor(i).config;
}
constexpr EventOutputConfig NodeBundle::event_output_config(size_t i) const {
  return event_output_descriptor(i).config;
}

constexpr std::string_view NodeBundle::type_identity() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [](auto const &payload) -> std::string_view {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) {
          return "Boundary";
        } else {
          return payload.type_identity.value;
        }
      },
      *_payload);
}

constexpr size_t NodeBundle::sample_input_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit([](auto const &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) return payload.sample_outputs.size();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) return payload.ports.sample_inputs.size();
    else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) return payload.sample_input_configs.size();
    else return payload.sample_input_count;
  }, *_payload);
}
constexpr size_t NodeBundle::event_input_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit([](auto const &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) return payload.event_outputs.size();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) return payload.ports.event_input_configs.size();
    else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) return payload.event_input_configs.size();
    else return payload.event_input_count;
  }, *_payload);
}
constexpr size_t NodeBundle::event_output_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit([](auto const &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) return payload.event_inputs.size();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) return payload.ports.event_output_configs.size();
    else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) return payload.event_output_configs.size();
    else return payload.event_output_count;
  }, *_payload);
}

constexpr size_t NodeBundle::sample_input_index(std::string_view name) const {
  return index_for_name(sample_input_count(),
      [&](size_t i) { return sample_input_config(i).name == name; }, name);
}
constexpr size_t NodeBundle::sample_output_index(std::string_view name) const {
  return index_for_name(sample_output_count(),
      [&](size_t i) { return sample_output_config(i).name == name; }, name);
}
constexpr size_t NodeBundle::event_input_index(std::string_view name) const {
  return index_for_name(event_input_count(),
      [&](size_t i) { return event_input_config(i).name == name; }, name);
}
constexpr size_t NodeBundle::event_output_index(std::string_view name) const {
  return index_for_name(event_output_count(),
      [&](size_t i) { return event_output_config(i).name == name; }, name);
}

constexpr void NodeBundle::import_into(
    size_t node_bundle_offset, size_t detach_id_offset) {
  if (!_payload) details::error("empty NodeBundle");
  std::visit([&](auto &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
      if (payload.deferred_detach) {
        payload.deferred_detach->id += detach_id_offset;
      }
    } else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
      for (auto &member : payload.member_bundles) member += node_bundle_offset;
    } else if constexpr (std::is_same_v<Bundle, SubgraphNodeBundle>) {
      payload.boundary += node_bundle_offset;
      payload.child_begin += node_bundle_offset;
    }
  }, *_payload);
  _virtual_node_handles.clear();
}

constexpr std::vector<size_t> &NodeBundle::virtual_node_handles() {
  return _virtual_node_handles;
}
constexpr std::vector<size_t> const &NodeBundle::virtual_node_handles() const {
  return _virtual_node_handles;
}
constexpr NodeSourceAnnotations &NodeBundle::source_annotations() {
  return _source_annotations;
}
constexpr NodeSourceAnnotations const &NodeBundle::source_annotations() const {
  return _source_annotations;
}

constexpr NodeBundleHandle GraphBuilderNodeBundles::append_tiled(
    std::span<NodeBundleHandle const> members,
    ChannelLayout promoted_channel_layout) {
  if (members.empty()) details::error("tiled NodeBundle requires members");
  auto const &first = bundle(members.front());
  if (!first.is_concrete()) details::error("tiled NodeBundle members must be concrete");

  NodeBundle::TiledNodeBundle payload;
  payload.member_bundles.assign(members.begin(), members.end());
  payload.type_identity.value = std::string(first.type_identity());

  for (size_t i = 0; i < first.sample_input_count(); ++i) {
    auto config = first.sample_input_config(i);
    config.channel_layout = promoted_channel_layout;
    payload.sample_input_configs.push_back(std::move(config));
  }
  for (size_t i = 0; i < first.sample_output_count(); ++i) {
    auto config = first.sample_output_config(i);
    config.channel_layout = promoted_channel_layout;
    payload.sample_output_configs.push_back(std::move(config));
  }
  for (size_t i = 0; i < first.event_input_count(); ++i)
    payload.event_input_configs.push_back(first.event_input_config(i));
  for (size_t i = 0; i < first.event_output_count(); ++i)
    payload.event_output_configs.push_back(first.event_output_config(i));

  for (auto const member : members.subspan(1)) {
    auto const &candidate = bundle(member);
    if (!candidate.is_concrete()) details::error("tiled NodeBundle members must be concrete");
    if (candidate.sample_input_count() != first.sample_input_count() ||
        candidate.sample_output_count() != first.sample_output_count() ||
        candidate.event_input_count() != first.event_input_count() ||
        candidate.event_output_count() != first.event_output_count()) {
      details::error("tiled NodeBundle members do not expose the same ports");
    }
  }

  auto const handle = _bundles.size();
  _bundles.push_back(NodeBundle(std::move(payload)));
  return handle;
}

constexpr NodeBundleHandle GraphBuilderNodeBundles::append_subgraph(
    NodeBundleHandle boundary, size_t child_begin, size_t child_count,
    std::string_view kind) {
  auto const &boundary_bundle = bundle(boundary);
  if (!boundary_bundle.is_boundary()) details::error("subgraph boundary is not a BoundaryNodeBundle");
  if (child_begin + child_count > _bundles.size()) details::error("subgraph child bundle range is out of bounds");

  auto const sample_input_count = boundary_bundle.boundary_sample_inputs().size();
  auto const sample_output_count = boundary_bundle.boundary_sample_outputs().size();
  auto const event_input_count = boundary_bundle.boundary_event_inputs().size();
  auto const event_output_count = boundary_bundle.boundary_event_outputs().size();
  NodeBundle::SubgraphNodeBundle payload{
      .boundary = boundary,
      .child_begin = child_begin,
      .child_count = child_count,
      .kind = std::string(kind),
      .type_identity = NodeTypeIdentity{.value = "lowered-subgraph:" + std::string(kind)},
      .sample_input_count = sample_input_count,
      .sample_output_count = sample_output_count,
      .event_input_count = event_input_count,
      .event_output_count = event_output_count,
  };

  auto const handle = _bundles.size();
  _bundles.push_back(NodeBundle(std::move(payload)));
  return handle;
}

constexpr EventInputPortDescriptor
GraphBuilderNodeBundles::resolve_event_input(NodeBundlePortId id) const {
  if (id.port_kind != PortKind::event) details::error("event input address has sample kind");
  auto const &candidate = bundle(id.node_bundle_handle);
  if (auto boundary = candidate.subgraph_boundary_handle()) {
    return descriptor<EventInputPortDescriptor>(
        bundle(*boundary).boundary_event_inputs(), id.port_ordinal);
  }
  return candidate.event_input_descriptor(id.port_ordinal);
}
constexpr EventOutputPortDescriptor
GraphBuilderNodeBundles::resolve_event_output(NodeBundlePortId id) const {
  if (id.port_kind != PortKind::event) details::error("event output address has sample kind");
  auto const &candidate = bundle(id.node_bundle_handle);
  if (auto boundary = candidate.subgraph_boundary_handle()) {
    return descriptor<EventOutputPortDescriptor>(
        bundle(*boundary).boundary_event_outputs(), id.port_ordinal);
  }
  return candidate.event_output_descriptor(id.port_ordinal);
}

constexpr std::vector<EventInputPortId>
GraphBuilderNodeBundles::event_input_ports(NodeBundlePortId id) const {
  (void)resolve_event_input(id);
  return {{id.node_bundle_handle, id.port_ordinal}};
}
constexpr std::vector<EventOutputPortId>
GraphBuilderNodeBundles::event_output_ports(NodeBundlePortId id) const {
  (void)resolve_event_output(id);
  return {{id.node_bundle_handle, id.port_ordinal}};
}

constexpr NodeBundleHandle GraphBuilderNodeBundles::tiled_member(
    NodeBundleHandle handle, size_t channel) const {
  auto const members = bundle(handle).tiled_members();
  if (channel >= members.size()) details::error("tiled NodeBundle channel is out of bounds");
  return members[channel];
}

constexpr NodeLifetime const &GraphBuilderNodeBundles::concrete_lifetime(
    NodeBundleHandle handle) const {
  auto const &b = bundle(handle);
  auto const *payload = b._payload ? std::get_if<NodeBundle::ConcreteNodeBundle>(&*b._payload) : nullptr;
  if (!payload) details::error("NodeBundle is not concrete");
  return payload->lifetime;
}

constexpr ReflectedNodeDescription
GraphBuilderNodeBundles::materialize_concrete_description(
    NodeBundleHandle handle) const {
  auto const &b = bundle(handle);
  auto const *payload = b._payload
      ? std::get_if<NodeBundle::ConcreteNodeBundle>(&*b._payload)
      : nullptr;
  if (!payload) details::error("NodeBundle is not concrete");
  return ReflectedNodeDescription{
      .ports = payload->ports,
      .operations = payload->operations,
      .type_name = payload->reflected_type_name,
      .internal_latency_samples = payload->internal_latency_samples,
      .maximum_block_size = payload->maximum_block_size,
      .default_ttl_samples = payload->default_ttl_samples,
      .block_skippable = payload->block_skippable,
      .static_sample_value = payload->static_sample_value,
  };
}

constexpr SemanticSubgraphInfo GraphBuilderNodeBundles::subgraph_info(
    NodeBundleHandle handle) const {
  auto const &b = bundle(handle);
  auto const *payload = b._payload ? std::get_if<NodeBundle::SubgraphNodeBundle>(&*b._payload) : nullptr;
  if (!payload) details::error("NodeBundle is not a subgraph");
  return SemanticSubgraphInfo{
      .boundary = payload->boundary,
      .child_begin = payload->child_begin,
      .child_count = payload->child_count,
      .kind = payload->kind,
      .lifetime = payload->lifetime,
  };
}

constexpr void GraphBuilderNodeBundles::apply_ttl(
    NodeBundleHandle handle, size_t ttl_samples) {
  auto apply_one = [&](NodeBundleHandle candidate_handle) {
    auto &candidate = bundle(candidate_handle);
    if (auto *concrete = candidate._payload
            ? std::get_if<NodeBundle::ConcreteNodeBundle>(&*candidate._payload)
            : nullptr) {
      concrete->lifetime.ttl_samples = ttl_samples;
      return;
    }
    if (auto *subgraph = candidate._payload
            ? std::get_if<NodeBundle::SubgraphNodeBundle>(&*candidate._payload)
            : nullptr) {
      subgraph->lifetime.ttl_samples = ttl_samples;
    }
  };

  auto &b = bundle(handle);
  if (b.is_concrete()) {
    apply_one(handle);
    return;
  }
  if (auto *tiled = b._payload
          ? std::get_if<NodeBundle::TiledNodeBundle>(&*b._payload)
          : nullptr) {
    for (auto const member : tiled->member_bundles) apply_one(member);
    return;
  }
  if (auto *subgraph = b._payload
          ? std::get_if<NodeBundle::SubgraphNodeBundle>(&*b._payload)
          : nullptr) {
    subgraph->lifetime.ttl_samples = ttl_samples;
    auto const end = subgraph->child_begin + subgraph->child_count;
    for (auto child = subgraph->child_begin; child < end; ++child) {
      apply_one(child);
    }
    return;
  }
  details::error("cannot apply ttl to a boundary NodeBundle");
}

constexpr size_t GraphBuilderNodeBundles::import_child(
    GraphBuilderNodeBundles const &child, size_t detach_id_offset) {
  auto const bundle_offset = _bundles.size();
  _bundles.reserve(_bundles.size() + child._bundles.size());
  for (auto imported : child._bundles) {
    imported.import_into(bundle_offset, detach_id_offset);
    _bundles.push_back(std::move(imported));
  }
  return bundle_offset;
}

template<class Visitor>
constexpr void GraphBuilderNodeBundles::for_each_authored_bundle(
    Visitor&& visitor) const {
  for (auto const& bundle : _bundles) {
    AuthoredNodeBundleView view{
        .virtual_node_handles = bundle._virtual_node_handles,
        .source_infos = bundle._source_annotations.infos,
    };
    std::visit([&](auto const& payload) {
      using Payload = std::remove_cvref_t<decltype(payload)>;
      if constexpr (std::same_as<Payload, NodeBundle::ConcreteNodeBundle>) {
        view.kind = AuthoredNodeBundleKind::concrete;
        view.ports = &payload.ports;
        view.operations = &payload.operations;
        view.lifetime = &payload.lifetime;
        view.type_identity = &payload.type_identity.value;
        view.reflected_type_name = &payload.reflected_type_name;
        view.internal_latency_samples = payload.internal_latency_samples;
        view.maximum_block_size = payload.maximum_block_size;
        view.default_ttl_samples = &payload.default_ttl_samples;
        view.block_skippable = payload.block_skippable;
        view.static_sample_value = &payload.static_sample_value;
        view.deferred_detach = &payload.deferred_detach;
      } else if constexpr (std::same_as<Payload, NodeBundle::TiledNodeBundle>) {
        view.kind = AuthoredNodeBundleKind::tiled;
        view.tiled_members = std::span<NodeBundleHandle const>{
            payload.member_bundles};
        view.type_identity = &payload.type_identity.value;
        view.sample_input_configs = std::span<InputConfig const>{
            payload.sample_input_configs};
        view.sample_output_configs = std::span<OutputConfig const>{
            payload.sample_output_configs};
        view.event_input_configs = std::span<EventInputConfig const>{
            payload.event_input_configs};
        view.event_output_configs = std::span<EventOutputConfig const>{
            payload.event_output_configs};
      } else if constexpr (std::same_as<Payload, NodeBundle::BoundaryNodeBundle>) {
        view.kind = AuthoredNodeBundleKind::boundary;
        view.sample_input_configs = std::span<InputConfig const>{
            payload.sample_inputs};
        view.sample_output_configs = std::span<OutputConfig const>{
            payload.sample_outputs};
        view.event_input_configs = std::span<EventInputConfig const>{
            payload.event_inputs};
        view.event_output_configs = std::span<EventOutputConfig const>{
            payload.event_outputs};
      } else {
        view.kind = AuthoredNodeBundleKind::subgraph;
        view.subgraph_boundary = payload.boundary;
        view.subgraph_child_begin = payload.child_begin;
        view.subgraph_child_count = payload.child_count;
        view.subgraph_kind = &payload.kind;
        view.lifetime = &payload.lifetime;
        view.type_identity = &payload.type_identity.value;
        view.subgraph_sample_input_count = payload.sample_input_count;
        view.subgraph_sample_output_count = payload.sample_output_count;
        view.subgraph_event_input_count = payload.event_input_count;
        view.subgraph_event_output_count = payload.event_output_count;
      }
    }, *bundle._payload);
    visitor(view);
  }
}

constexpr GraphBuilderNodeBundles GraphBuilderNodeBundles::from_authored_records(
    std::span<AuthoredNodeBundleRecord const> records) {
  GraphBuilderNodeBundles result;
  result._bundles.reserve(records.size());
  for (auto const& record : records) {
    NodeBundle bundle;
    switch (record.kind) {
    case AuthoredNodeBundleKind::concrete:
      bundle = NodeBundle(NodeBundle::ConcreteNodeBundle{
          .ports = record.ports,
          .operations = record.operations,
          .lifetime = record.lifetime,
          .type_identity = {.value = record.type_identity},
          .reflected_type_name = record.reflected_type_name,
          .internal_latency_samples = record.internal_latency_samples,
          .maximum_block_size = record.maximum_block_size,
          .default_ttl_samples = record.default_ttl_samples,
          .block_skippable = record.block_skippable,
          .static_sample_value = record.static_sample_value,
          .deferred_detach = record.deferred_detach,
      });
      break;
    case AuthoredNodeBundleKind::tiled:
      bundle = NodeBundle(NodeBundle::TiledNodeBundle{
          .member_bundles = record.tiled_members,
          .type_identity = {.value = record.type_identity},
          .sample_input_configs = record.sample_input_configs,
          .sample_output_configs = record.sample_output_configs,
          .event_input_configs = record.event_input_configs,
          .event_output_configs = record.event_output_configs,
      });
      break;
    case AuthoredNodeBundleKind::boundary:
      bundle = NodeBundle(NodeBundle::BoundaryNodeBundle{
          .sample_inputs = record.sample_input_configs,
          .sample_outputs = record.sample_output_configs,
          .event_inputs = record.event_input_configs,
          .event_outputs = record.event_output_configs,
      });
      break;
    case AuthoredNodeBundleKind::subgraph:
      bundle = NodeBundle(NodeBundle::SubgraphNodeBundle{
          .boundary = record.subgraph_boundary,
          .child_begin = record.subgraph_child_begin,
          .child_count = record.subgraph_child_count,
          .kind = record.subgraph_kind,
          .lifetime = record.lifetime,
          .type_identity = {.value = record.type_identity},
          .sample_input_count = record.subgraph_sample_input_count,
          .sample_output_count = record.subgraph_sample_output_count,
          .event_input_count = record.subgraph_event_input_count,
          .event_output_count = record.subgraph_event_output_count,
      });
      break;
    }
    bundle._virtual_node_handles = record.virtual_node_handles;
    bundle._source_annotations.infos = record.source_infos;
    result._bundles.push_back(std::move(bundle));
  }
  return result;
}
} // namespace iv
