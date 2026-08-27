#pragma once

#include <intravenous/graph/names.h>
#include <intravenous/graph/builder/stored_node.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
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
    ReflectedNodeOperations operations{};
    NodeLifetime lifetime{};
    NodeTypeIdentity type_identity{};
    size_t internal_latency_samples = 0;
    size_t maximum_block_size = MAX_BLOCK_SIZE;
    std::optional<size_t> default_ttl_samples{};
    bool block_skippable = false;
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
  constexpr ConcreteNode lowered_concrete(NodeBundleHandle) const;
  constexpr SemanticSubgraphInfo subgraph_info(NodeBundleHandle) const;

  constexpr size_t size() const { return _bundles.size(); }
  constexpr void apply_ttl(NodeBundleHandle, size_t ttl_samples);
  constexpr size_t import_child(
      GraphBuilderNodeBundles const &, size_t detach_id_offset);

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
      .internal_latency_samples = description.internal_latency_samples,
      .maximum_block_size = description.maximum_block_size,
      .default_ttl_samples = description.default_ttl_samples,
      .block_skippable = description.block_skippable,
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
      .internal_latency_samples = lowered.internal_latency_samples,
      .maximum_block_size = lowered.maximum_block_size,
      .default_ttl_samples = lowered.default_ttl_samples,
      .block_skippable = lowered.block_skippable,
  };
  auto const handle = _bundles.size();
  _bundles.push_back(NodeBundle(std::move(payload)));
  return handle;
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
  for (NodeBundleHandle handle = 0; handle < _bundles.size(); ++handle) {
    auto const &candidate = bundle(handle);
    for (size_t port = 0; port < candidate.sample_output_count(); ++port) {
      NodeBundlePortId const id {handle, PortKind::sample, port};
      if (resolve_sample_output(id).config.channel_layout.channel_type != type)
        continue;
      auto const candidate_channels = sample_output_channels(id);
      if (candidate_channels.size() != channels.size()) continue;
      bool equal = true;
      for (size_t channel = 0; channel < channels.size(); ++channel) {
        if (candidate_channels[channel] != channels[channel]) {
          equal = false;
          break;
        }
      }
      if (equal) return id;
    }
  }
  return std::nullopt;
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

template <class Configs>
constexpr size_t index_for_name(
    Configs const &configs, std::string_view name) {
  std::optional<size_t> result;
  for (size_t ordinal = 0; ordinal < configs.size(); ++ordinal) {
    if (configs[ordinal].name != name) continue;
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
  std::vector<InputConfig> configs;
  configs.reserve(sample_input_count());
  for (size_t i = 0; i < sample_input_count(); ++i) configs.push_back(sample_input_config(i));
  return index_for_name(configs, name);
}
constexpr size_t NodeBundle::sample_output_index(std::string_view name) const {
  std::vector<OutputConfig> configs;
  configs.reserve(sample_output_count());
  for (size_t i = 0; i < sample_output_count(); ++i) configs.push_back(sample_output_config(i));
  return index_for_name(configs, name);
}
constexpr size_t NodeBundle::event_input_index(std::string_view name) const {
  std::vector<EventInputConfig> configs;
  configs.reserve(event_input_count());
  for (size_t i = 0; i < event_input_count(); ++i) configs.push_back(event_input_config(i));
  return index_for_name(configs, name);
}
constexpr size_t NodeBundle::event_output_index(std::string_view name) const {
  std::vector<EventOutputConfig> configs;
  configs.reserve(event_output_count());
  for (size_t i = 0; i < event_output_count(); ++i) configs.push_back(event_output_config(i));
  return index_for_name(configs, name);
}

constexpr void NodeBundle::import_into(
    size_t node_bundle_offset, size_t detach_id_offset) {
  if (!_payload) details::error("empty NodeBundle");
  std::visit([&](auto &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
      payload.operations =
          payload.operations.apply_detach_id_offset(detach_id_offset);
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

constexpr ConcreteNode GraphBuilderNodeBundles::lowered_concrete(
    NodeBundleHandle handle) const {
  auto const &b = bundle(handle);
  auto const *payload = b._payload ? std::get_if<NodeBundle::ConcreteNodeBundle>(&*b._payload) : nullptr;
  if (!payload) details::error("NodeBundle is not concrete");
  return ConcreteNode{
      .ports = payload->ports,
      .operations = payload->operations,
      .lifetime = payload->lifetime,
      .type_identity = payload->type_identity,
      .internal_latency_samples = payload->internal_latency_samples,
      .maximum_block_size = payload->maximum_block_size,
      .default_ttl_samples = payload->default_ttl_samples,
      .block_skippable = payload->block_skippable,
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
} // namespace iv
