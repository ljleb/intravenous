#pragma once

#include <intravenous/basic_nodes/routing.h>
#include <intravenous/graph/error.h>
#include <intravenous/graph/names.h>
#include <intravenous/graph/port_ids.h>
#include <intravenous/graph/builder/stored_node.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
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
class GraphBuilderState;
template<class Config>
struct SamplePortDescriptor {
  Config config{};
};

template<class Config>
struct EventPortDescriptor {
  Config config{};
};

using SampleInputPortDescriptor = SamplePortDescriptor<SampleInputConfig>;
using SampleOutputPortDescriptor = SamplePortDescriptor<SampleOutputConfig>;
using EventInputPortDescriptor = EventPortDescriptor<EventInputConfig>;
using EventOutputPortDescriptor = EventPortDescriptor<EventOutputConfig>;

class NodeBundle {
  struct ConcreteNodeBundle {
    NodePorts ports{};
    ReflectedNodeOperations operations{};
    std::shared_ptr<void const> node_storage{};
    std::shared_ptr<NodeStateStructures const> state_structures_storage{};
    NodeConfigRelocations config_relocations{};
    NodeCodeKey code_key{};
    std::optional<RegisteredNodeTypeIdentity> registered_node_type_identity{};
    size_t node_size = 0;
    size_t node_alignment = 1;
    NodeLifetime lifetime{};
    NodeTypeIdentity type_identity{};
    std::string reflected_type_name{};
    size_t internal_latency_samples = 0;
    size_t maximum_block_size = MAX_BLOCK_SIZE;
    std::optional<size_t> default_ttl_samples{};
    bool block_skippable = false;
    std::optional<Sample> static_sample_value{};
  };

  struct TiledNodeBundle {
    std::vector<NodeBundleHandle> member_bundles{};
    NodeTypeIdentity type_identity{};
    NodePorts ports{};
  };

  struct BoundaryNodeBundle {
    NodePorts ports{};
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

  constexpr NodePorts const& boundary_ports() const;
  constexpr std::vector<SampleInputConfig> boundary_sample_inputs() const;
  constexpr std::vector<SampleOutputConfig> boundary_sample_outputs() const;
  constexpr std::vector<EventInputConfig> boundary_event_inputs() const;
  constexpr std::vector<EventOutputConfig> boundary_event_outputs() const;
  constexpr size_t append_boundary_sample_input(SampleInputConfig);
  constexpr size_t append_boundary_sample_output(SampleOutputConfig);
  constexpr size_t append_boundary_event_input(EventInputConfig);
  constexpr size_t append_boundary_event_output(EventOutputConfig);
  constexpr void clear_boundary_event_outputs();

  constexpr ChannelLayout sample_input_layout(size_t) const;
  constexpr ChannelLayout sample_output_layout(size_t) const;
  constexpr SampleInputConfig sample_input_config(size_t) const;
  constexpr SampleOutputConfig sample_output_config(size_t) const;
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
  constexpr NodeBundlePortId input_port_at(
      NodeBundleHandle, size_t) const;
  constexpr void import_into(size_t node_bundle_offset);

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
  friend class GraphBuilderState;
};

struct SemanticSubgraphInfo {
  NodeBundleHandle boundary = 0;
  size_t child_begin = 0;
  size_t child_count = 0;
  std::string kind{};
  NodeLifetime lifetime{};
};

enum class ConfiguredNodeBundleKind : std::uint8_t {
  concrete,
  tiled,
  boundary,
  subgraph,
};

// A public, lossless record of the semantic bundle.  It deliberately contains
// no NodeBundle implementation details, so the frozen module ABI can be
// defined in terms of this data rather than the variant used by GraphBuilder.
struct ConfiguredNodeBundleRecord {
  ConfiguredNodeBundleKind kind = ConfiguredNodeBundleKind::boundary;
  NodePorts ports{};
  ReflectedNodeOperations operations{};
  std::shared_ptr<void const> node_storage{};
  std::shared_ptr<NodeStateStructures const> state_structures_storage{};
  NodeCodeKey code_key{};
  std::optional<RegisteredNodeTypeIdentity> registered_node_type_identity{};
  size_t node_size = 0;
  size_t node_alignment = 1;
  NodeLifetime lifetime{};
  std::string type_identity{};
  std::string reflected_type_name{};
  size_t internal_latency_samples = 0;
  size_t maximum_block_size = MAX_BLOCK_SIZE;
  std::optional<size_t> default_ttl_samples{};
  bool block_skippable = false;
  std::optional<Sample> static_sample_value{};

  std::vector<NodeBundleHandle> tiled_members{};

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
// the duration of the for_each_configured_bundle callback, allowing freezing to
// promote GraphBuilder storage directly without materializing an owning record.
struct ConfiguredNodeBundleView {
  ConfiguredNodeBundleKind kind = ConfiguredNodeBundleKind::boundary;
  NodePorts const* ports = nullptr;
  ReflectedNodeOperations const* operations = nullptr;
  std::shared_ptr<void const> const* node_storage = nullptr;
  std::shared_ptr<NodeStateStructures const> const* state_structures_storage = nullptr;
  NodeConfigRelocations const* config_relocations = nullptr;
  NodeCodeKey const* code_key = nullptr;
  RegisteredNodeTypeIdentity const* registered_node_type_identity = nullptr;
  size_t node_size = 0;
  size_t node_alignment = 1;
  NodeLifetime const* lifetime = nullptr;
  std::string const* type_identity = nullptr;
  std::string const* reflected_type_name = nullptr;
  size_t internal_latency_samples = 0;
  size_t maximum_block_size = MAX_BLOCK_SIZE;
  std::optional<size_t> const* default_ttl_samples = nullptr;
  bool block_skippable = false;
  std::optional<Sample> const* static_sample_value = nullptr;

  std::span<NodeBundleHandle const> tiled_members{};

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

  constexpr NodeBundlePortId input_port_at(NodeBundleHandle, size_t) const;
  constexpr NodePorts const &typed_ports(NodeBundleHandle) const;
  constexpr NodeBundleHandle tiled_member(
      NodeBundleHandle, size_t channel) const;
  constexpr NodeLifetime const &concrete_lifetime(NodeBundleHandle) const;
  constexpr void set_registered_node_type_identity(
      NodeBundleHandle, RegisteredNodeTypeIdentity);
  constexpr ReflectedNodeDescription materialize_concrete_description(
      NodeBundleHandle) const;
  constexpr SemanticSubgraphInfo subgraph_info(NodeBundleHandle) const;

  constexpr size_t size() const { return _bundles.size(); }
  constexpr void apply_ttl(NodeBundleHandle, size_t ttl_samples);
  constexpr size_t import_child(GraphBuilderNodeBundles const &);
  template<class Visitor>
  constexpr void for_each_configured_bundle(Visitor&& visitor) const;
  static constexpr GraphBuilderNodeBundles from_configured_records(
      std::span<ConfiguredNodeBundleRecord const>);

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
  auto const sample_outputs = description.ports.sample_outputs();
  auto const event_outputs = description.ports.event_outputs();
  validate_output_port_configs(
      std::span<SampleOutputConfig const>(sample_outputs),
      description.type_name,
      "sample");
  validate_output_port_configs(
      std::span<EventOutputConfig const>(event_outputs),
      description.type_name,
      "event");
  for (auto const& output : event_outputs) {
    if (!is_valid_event_buffer_rate(output.max_events_per_sample)) {
      details::error(std::string(description.type_name)
          + ": event output max_events_per_sample must be finite and nonnegative");
    }
  }

  return ConcreteNode{
      .ports = std::move(description.ports),
      .operations = description.operations,
      .node_storage = std::move(description.node_storage),
      .state_structures_storage = std::move(description.state_structures_storage),
      .config_relocations = std::move(description.config_relocations),
      .code_key = description.code_key,
      .registered_node_type_identity = std::move(description.registered_node_type_identity),
      .node_size = description.node_size,
      .node_alignment = description.node_alignment,
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
  Node node(std::forward<Args>(args)...);
  return make_concrete_node(details::reflect_node(node));
}

constexpr NodeBundleHandle GraphBuilderNodeBundles::append_concrete(
    ConcreteNode lowered) {
  NodeBundle::ConcreteNodeBundle payload{
      .ports = std::move(lowered.ports),
      .operations = lowered.operations,
      .node_storage = std::move(lowered.node_storage),
      .state_structures_storage = std::move(lowered.state_structures_storage),
      .config_relocations = std::move(lowered.config_relocations),
      .code_key = lowered.code_key,
      .registered_node_type_identity = std::move(lowered.registered_node_type_identity),
      .node_size = lowered.node_size,
      .node_alignment = lowered.node_alignment,
      .lifetime = std::move(lowered.lifetime),
      .type_identity = std::move(lowered.type_identity),
      .reflected_type_name = std::string(lowered.reflected_type_name),
      .internal_latency_samples = lowered.internal_latency_samples,
      .maximum_block_size = lowered.maximum_block_size,
      .default_ttl_samples = lowered.default_ttl_samples,
      .block_skippable = lowered.block_skippable,
      .static_sample_value = lowered.static_sample_value,
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
      return payload.ports.sample_input_count();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>
                       || std::is_same_v<Bundle, TiledNodeBundle>)
      return payload.ports.sample_output_count();
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

constexpr NodeBundlePortId GraphBuilderNodeBundles::input_port_at(
    NodeBundleHandle handle, size_t position) const {
  auto const& candidate = bundle(handle);
  if (auto const boundary = candidate.subgraph_boundary_handle()) {
    auto const resolved = input_port_at(*boundary, position);
    return {handle, resolved.port_kind, resolved.port_ordinal};
  }
  return candidate.input_port_at(handle, position);
}

constexpr std::optional<NodeBundleHandle>
NodeBundle::subgraph_boundary_handle() const {
  if (!_payload) details::error("empty NodeBundle");
  auto const *subgraph = std::get_if<SubgraphNodeBundle>(&*_payload);
  return subgraph
      ? std::optional<NodeBundleHandle>(subgraph->boundary)
      : std::nullopt;
}

constexpr NodePorts const& NodeBundle::boundary_ports() const {
  auto const *boundary = _payload
      ? std::get_if<BoundaryNodeBundle>(&*_payload)
      : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->ports;
}

constexpr std::vector<SampleOutputConfig>
NodeBundle::boundary_sample_outputs() const {
  auto const *boundary = _payload
      ? std::get_if<BoundaryNodeBundle>(&*_payload)
      : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->ports.sample_outputs();
}

constexpr size_t NodeBundle::append_boundary_sample_output(
    SampleOutputConfig config) {
  auto *boundary = _payload
      ? std::get_if<BoundaryNodeBundle>(&*_payload)
      : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->ports.sample_output_count();
  boundary->ports.output_configs.push_back(make_output_config(config));
  return ordinal;
}

constexpr std::vector<SampleInputConfig>
NodeBundle::boundary_sample_inputs() const {
  auto const *boundary = _payload
      ? std::get_if<BoundaryNodeBundle>(&*_payload)
      : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->ports.sample_inputs();
}

constexpr SampleInputPortDescriptor
NodeBundle::sample_input_descriptor(size_t ordinal) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) -> SampleInputPortDescriptor {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) {
          auto const output = payload.ports.sample_output(ordinal);
          return {.config = SampleInputConfig{
              .name = output.name,
              .channel_layout = output.channel_layout,
              .access = inward_input_access(output.access),
          }};
        } else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
          return {.config = payload.ports.sample_input(ordinal)};
        } else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
          return {.config = payload.ports.sample_input(ordinal)};
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
          auto const input = payload.ports.sample_input(ordinal);
          return {.config = SampleOutputConfig{
              .name = input.name,
              .channel_layout = input.channel_layout,
              .access = inward_output_access(input.access),
          }};
        } else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
          return {.config = payload.ports.sample_output(ordinal)};
        } else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
          return {.config = payload.ports.sample_output(ordinal)};
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
  return EventOutputConfig{.name = config.name, .type = config.type, .access = inward_output_access(config.access)};
}

constexpr EventInputConfig inward_event_input_config(
    EventOutputConfig const &config) {
  return EventInputConfig{.name = config.name, .type = config.type, .access = inward_input_access(config.access)};
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
          return {.config = inward_event_input_config(
              payload.ports.event_output(i))};
        } else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
          return {.config = payload.ports.event_input(i)};
        } else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
          return {.config = payload.ports.event_input(i)};
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
          return {.config = inward_event_output_config(
              payload.ports.event_input(i))};
        } else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
          return {.config = payload.ports.event_output(i)};
        } else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
          return {.config = payload.ports.event_output(i)};
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

constexpr std::vector<EventInputConfig>
NodeBundle::boundary_event_inputs() const {
  auto const *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->ports.event_inputs();
}
constexpr std::vector<EventOutputConfig>
NodeBundle::boundary_event_outputs() const {
  auto const *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->ports.event_outputs();
}

constexpr size_t NodeBundle::append_boundary_sample_input(SampleInputConfig config) {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->ports.sample_input_count();
  boundary->ports.input_configs.push_back(make_input_config(config));
  return ordinal;
}
constexpr size_t NodeBundle::append_boundary_event_input(
    EventInputConfig config) {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->ports.event_input_count();
  boundary->ports.input_configs.push_back(make_input_config(config));
  return ordinal;
}
constexpr size_t NodeBundle::append_boundary_event_output(
    EventOutputConfig config) {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->ports.event_output_count();
  boundary->ports.output_configs.push_back(make_output_config(config));
  return ordinal;
}
constexpr void NodeBundle::clear_boundary_event_outputs() {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  std::erase_if(boundary->ports.output_configs, [](OutputConfig const& config) {
    return !is_sample(config);
  });
}

constexpr ChannelLayout NodeBundle::sample_input_layout(size_t i) const {
  return sample_input_descriptor(i).config.channel_layout;
}
constexpr ChannelLayout NodeBundle::sample_output_layout(size_t i) const {
  return sample_output_descriptor(i).config.channel_layout;
}
constexpr SampleInputConfig NodeBundle::sample_input_config(size_t i) const {
  return sample_input_descriptor(i).config;
}
constexpr SampleOutputConfig NodeBundle::sample_output_config(size_t i) const {
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
    if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
      return payload.ports.sample_output_count();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>
                       || std::is_same_v<Bundle, TiledNodeBundle>)
      return payload.ports.sample_input_count();
    else
      return payload.sample_input_count;
  }, *_payload);
}
constexpr size_t NodeBundle::event_input_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit([](auto const &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
      return payload.ports.event_output_count();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>
                       || std::is_same_v<Bundle, TiledNodeBundle>)
      return payload.ports.event_input_count();
    else
      return payload.event_input_count;
  }, *_payload);
}
constexpr size_t NodeBundle::event_output_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit([](auto const &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
      return payload.ports.event_input_count();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>
                       || std::is_same_v<Bundle, TiledNodeBundle>)
      return payload.ports.event_output_count();
    else
      return payload.event_output_count;
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

constexpr NodeBundlePortId NodeBundle::input_port_at(
    NodeBundleHandle handle, size_t position) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit([&](auto const& payload) -> NodeBundlePortId {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>
                  || std::is_same_v<Bundle, TiledNodeBundle>
                  || std::is_same_v<Bundle, BoundaryNodeBundle>) {
      return payload.ports.input_port_at(handle, position);
    } else {
      details::error("subgraph input order must resolve through its boundary");
    }
  }, *_payload);
}

constexpr void NodeBundle::import_into(size_t node_bundle_offset) {
  if (!_payload) details::error("empty NodeBundle");
  std::visit([&](auto &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
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
  if (!first.is_concrete() && !first.is_subgraph()) {
    details::error(
        "tiled NodeBundle members must be concrete nodes or subgraphs");
  }

  auto logical_ports_of = [&](NodeBundleHandle handle) -> NodePorts const& {
    auto const& candidate = bundle(handle);
    if (candidate.is_concrete()) return candidate.concrete_ports();
    auto const boundary_handle = candidate.subgraph_boundary_handle();
    if (!boundary_handle) {
      details::error("tiled NodeBundle member has no logical port interface");
    }
    auto const& boundary = bundle(*boundary_handle);
    auto const* payload = std::get_if<NodeBundle::BoundaryNodeBundle>(
        &*boundary._payload);
    if (!payload) details::error("subgraph boundary is not a BoundaryNodeBundle");
    return payload->ports;
  };

  auto same_input = [](InputConfig const& lhs, InputConfig const& rhs) {
    if (lhs.name != rhs.name || lhs.access != rhs.access
        || is_sample(lhs) != is_sample(rhs)) return false;
    if (is_sample(lhs)) {
      auto const& a = sample_properties(lhs);
      auto const& b = sample_properties(rhs);
      return a.channel_layout == b.channel_layout
          && a.neutral_value.value == b.neutral_value.value
          && a.default_value.value == b.default_value.value
          && a.min.value == b.min.value
          && a.max.value == b.max.value;
    }
    return event_properties(lhs).type == event_properties(rhs).type;
  };
  auto same_output = [](OutputConfig const& lhs, OutputConfig const& rhs) {
    if (lhs.name != rhs.name || lhs.access != rhs.access
        || is_sample(lhs) != is_sample(rhs)) return false;
    if (is_sample(lhs)) {
      auto const& a = sample_properties(lhs);
      auto const& b = sample_properties(rhs);
      return a.channel_layout == b.channel_layout;
    }
    return event_properties(lhs).type == event_properties(rhs).type;
  };
  auto validate_mono_samples = [](NodePorts const& ports) {
    for (InputConfig const& input : ports.inputs()) {
      if (is_sample(input)
          && sample_properties(input).channel_layout.channel_type
              != ChannelTypeId::mono) {
        details::error(
            "tiled NodeBundle members must expose only mono sample inputs");
      }
    }
    for (OutputConfig const& output : ports.outputs()) {
      if (is_sample(output)
          && sample_properties(output).channel_layout.channel_type
              != ChannelTypeId::mono) {
        details::error(
            "tiled NodeBundle members must expose only mono sample outputs");
      }
    }
  };

  auto const& first_ports = logical_ports_of(members.front());
  validate_mono_samples(first_ports);

  NodeBundle::TiledNodeBundle payload;
  payload.member_bundles.assign(members.begin(), members.end());
  payload.type_identity.value = std::string(first.type_identity());
  payload.ports = first_ports;
  for (InputConfig& input : payload.ports.input_configs) {
    if (is_sample(input)) {
      std::get<SampleInputProperties>(input.kind).channel_layout =
          promoted_channel_layout;
    }
  }
  for (OutputConfig& output : payload.ports.output_configs) {
    if (is_sample(output)) {
      std::get<SampleOutputProperties>(output.kind).channel_layout =
          promoted_channel_layout;
    }
  }

  for (auto const member : members.subspan(1)) {
    auto const &candidate = bundle(member);
    if (candidate.is_concrete() != first.is_concrete()
        || candidate.is_subgraph() != first.is_subgraph()) {
      details::error(
          "tiled NodeBundle members must all be concrete nodes or all be subgraphs");
    }
    auto const& candidate_ports = logical_ports_of(member);
    validate_mono_samples(candidate_ports);
    if (!std::ranges::equal(
            first_ports.inputs(), candidate_ports.inputs(), same_input)
        || !std::ranges::equal(
            first_ports.outputs(), candidate_ports.outputs(), same_output)) {
      details::error(
          "tiled NodeBundle members do not expose equivalent port configurations");
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

constexpr void GraphBuilderNodeBundles::set_registered_node_type_identity(
    NodeBundleHandle handle, RegisteredNodeTypeIdentity identity) {
  auto& target = bundle(handle);
  if (auto* concrete = target._payload
          ? std::get_if<NodeBundle::ConcreteNodeBundle>(&*target._payload)
          : nullptr) {
    concrete->registered_node_type_identity = std::move(identity);
    return;
  }
  if (auto* tiled = target._payload
          ? std::get_if<NodeBundle::TiledNodeBundle>(&*target._payload)
          : nullptr) {
    for (auto const member : tiled->member_bundles) {
      set_registered_node_type_identity(member, identity);
    }
    return;
  }
  details::error("registered node type identity requires a concrete or tiled node");
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
      .node_storage = payload->node_storage,
      .state_structures_storage = payload->state_structures_storage,
      .config_relocations = payload->config_relocations,
      .code_key = payload->code_key,
      .registered_node_type_identity = payload->registered_node_type_identity,
      .node_size = payload->node_size,
      .node_alignment = payload->node_alignment,
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
    GraphBuilderNodeBundles const &child) {
  auto const bundle_offset = _bundles.size();
  _bundles.reserve(_bundles.size() + child._bundles.size());
  for (auto imported : child._bundles) {
    imported.import_into(bundle_offset);
    _bundles.push_back(std::move(imported));
  }
  return bundle_offset;
}

template<class Visitor>
constexpr void GraphBuilderNodeBundles::for_each_configured_bundle(
    Visitor&& visitor) const {
  for (auto const& bundle : _bundles) {
    ConfiguredNodeBundleView view{
        .virtual_node_handles = bundle._virtual_node_handles,
        .source_infos = bundle._source_annotations.infos,
    };
    std::visit([&](auto const& payload) {
      using Payload = std::remove_cvref_t<decltype(payload)>;
      if constexpr (std::same_as<Payload, NodeBundle::ConcreteNodeBundle>) {
        view.kind = ConfiguredNodeBundleKind::concrete;
        view.ports = &payload.ports;
        view.operations = &payload.operations;
        view.node_storage = &payload.node_storage;
        view.state_structures_storage = &payload.state_structures_storage;
        view.config_relocations = &payload.config_relocations;
        view.code_key = &payload.code_key;
        if (payload.registered_node_type_identity) {
          view.registered_node_type_identity =
              std::addressof(*payload.registered_node_type_identity);
        }
        view.node_size = payload.node_size;
        view.node_alignment = payload.node_alignment;
        view.lifetime = &payload.lifetime;
        view.type_identity = &payload.type_identity.value;
        view.reflected_type_name = &payload.reflected_type_name;
        view.internal_latency_samples = payload.internal_latency_samples;
        view.maximum_block_size = payload.maximum_block_size;
        view.default_ttl_samples = &payload.default_ttl_samples;
        view.block_skippable = payload.block_skippable;
        view.static_sample_value = &payload.static_sample_value;
      } else if constexpr (std::same_as<Payload, NodeBundle::TiledNodeBundle>) {
        view.kind = ConfiguredNodeBundleKind::tiled;
        view.tiled_members = std::span<NodeBundleHandle const>{
            payload.member_bundles};
        view.type_identity = &payload.type_identity.value;
        view.ports = &payload.ports;
      } else if constexpr (std::same_as<Payload, NodeBundle::BoundaryNodeBundle>) {
        view.kind = ConfiguredNodeBundleKind::boundary;
        view.ports = &payload.ports;
      } else {
        view.kind = ConfiguredNodeBundleKind::subgraph;
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

constexpr GraphBuilderNodeBundles GraphBuilderNodeBundles::from_configured_records(
    std::span<ConfiguredNodeBundleRecord const> records) {
  GraphBuilderNodeBundles result;
  result._bundles.reserve(records.size());
  for (auto const& record : records) {
    NodeBundle bundle;
    switch (record.kind) {
    case ConfiguredNodeBundleKind::concrete: {
      auto operations = record.operations;
      // The archive reader owns this structure while reconstructing records.
      // Once the bundle takes its shared ownership, its runtime callback must
      // point at that durable copy rather than the soon-to-be-destroyed record.
      operations.runtime.state_structures = record.state_structures_storage
          ? record.state_structures_storage.get()
          : nullptr;
      bundle = NodeBundle(NodeBundle::ConcreteNodeBundle{
          .ports = record.ports,
          .operations = operations,
          .node_storage = record.node_storage,
          .state_structures_storage = record.state_structures_storage,
          .code_key = record.code_key,
          .registered_node_type_identity = record.registered_node_type_identity,
          .node_size = record.node_size,
          .node_alignment = record.node_alignment,
          .lifetime = record.lifetime,
          .type_identity = {.value = record.type_identity},
          .reflected_type_name = record.reflected_type_name,
          .internal_latency_samples = record.internal_latency_samples,
          .maximum_block_size = record.maximum_block_size,
          .default_ttl_samples = record.default_ttl_samples,
          .block_skippable = record.block_skippable,
          .static_sample_value = record.static_sample_value,
      });
      break;
    }
    case ConfiguredNodeBundleKind::tiled:
      bundle = NodeBundle(NodeBundle::TiledNodeBundle{
          .member_bundles = record.tiled_members,
          .type_identity = {.value = record.type_identity},
          .ports = record.ports,
      });
      break;
    case ConfiguredNodeBundleKind::boundary:
      bundle = NodeBundle(NodeBundle::BoundaryNodeBundle{
          .ports = record.ports,
      });
      break;
    case ConfiguredNodeBundleKind::subgraph:
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
