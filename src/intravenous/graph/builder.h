#pragma once
#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/channel_ports.h>
#include <intravenous/graph/builder/annotations.h>
#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/detach.hpp>
#include <intravenous/graph/builder/finalize.hpp>
#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/builder/node_refs.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/builder/public_ports.hpp>
#include <intravenous/graph/builder/subgraphs.hpp>
#include <intravenous/graph/builder/virtual_nodes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cxxabi.h>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace iv {
class GraphRuntimeBindings;
class GraphBuilder;
namespace details {
    struct GraphBuilderTestAccess;
}

class GraphBuilder {
  friend class NodeRef;
  template<class Derived> friend class NodeRefCrtp;
  template<class Node, class PortProjection> friend class TypedNodeRef;
  friend struct SamplePortRef;
  friend struct EventPortRef;
  friend class GraphBuilderChildEmbedder;
  friend class GraphBuilderConnections;
  friend class GraphBuilderDetach;
  friend class GraphBuilderAnnotations;
  friend class GraphBuilderVirtualNodes;
  friend class GraphBuilderPublicPorts;
  friend class SubgraphBuilder;
  friend struct details::GraphBuilderTestAccess;

  GraphBuilderIdentity _identity;
  GraphBuilderNodeBundles _node_bundles;
  GraphBuilderConnections _connections;
  GraphBuilderPublicPorts _public_ports;
  GraphBuilderDetach _detach;
  GraphBuilderAnnotations _annotations;
  GraphBuilderVirtualNodes _virtual_nodes;

  constexpr explicit GraphBuilder(GraphBuilderIdentity identity);
  GraphBuilder derive_nested_builder();
  NodeRef embed_subgraph(GraphBuilder const& child, std::string_view kind = "Subgraph");
  constexpr PublicSampleInputRef input_named(std::string_view name, Sample default_value,
                                   std::optional<Sample> min,
                                   std::optional<Sample> max);
  PublicEventInputRef event_input_named(std::string_view name, EventTypeId type);

public:
  constexpr GraphBuilder();
  PublicSampleInputRef input();
  template<fixed_string Name>
  constexpr PublicSampleInputRef input(Sample default_value = 0.0,
                             std::optional<Sample> min = std::nullopt,
                             std::optional<Sample> max = std::nullopt) {
    return input_named(Name.view(), default_value, min, max);
  }
  PublicSampleInputRef input(Sample default_value,
                             std::optional<Sample> min = std::nullopt,
                             std::optional<Sample> max = std::nullopt);
  template<fixed_string Name>
  PublicEventInputRef event_input(EventTypeId type) { return event_input_named(Name.view(), type); }
  PublicEventInputRef event_input(EventTypeId type);

  void annotate_public_sample_input_source_info(PublicSampleInputRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_public_event_input_source_info(PublicEventInputRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_public_sample_output_source_info(std::span<SourceInfo const> infos);
  void annotate_public_event_output_source_info(std::span<SourceInfo const> infos);

  template<class Config>
  static constexpr void validate_output_port_configs(
      std::span<Config const> configs,
      std::string_view node_label,
      std::string_view kind);
  template<class Node, class... Args>
  constexpr details::node_ref_for_t<Node> node(Args&&... args);
  template<class Node, class ChannelType, class... Args>
  constexpr auto node(Args&&... args);
  template<class ChannelType, class... Refs> constexpr auto tile(Refs&&... refs);
  template<auto Module> NodeRef module(std::string_view kind = "Module");

  template<class... Refs> void event_outputs(Refs&&... refs);
  void event_outputs(std::span<EventOutputRefConfig const> refs);
  template<class Fn> NodeRef subgraph(Fn&& fn, std::string_view kind = "Subgraph");
  template<class... Refs> constexpr void outputs(Refs&&... refs);
  constexpr void outputs(std::initializer_list<NamedRef> refs);
  void outputs(std::span<OutputRefConfig const> refs);
  constexpr void outputs(std::span<NamedRef const> refs);

  using VacantSampleInput = GraphBuilderVacantSampleInput;
  using VacantEventInput = GraphBuilderVacantEventInput;
  using RootNodeBuildResult = GraphBuilderRootNodeBuildResult;
  using VacantInputs = GraphBuilderVacantInputs;
  using VirtualSampleInput = GraphBuilderVirtualSampleInput;
  using VirtualEventInput = GraphBuilderVirtualEventInput;
  using VirtualInputs = GraphBuilderVirtualInputs;
  using VirtualSampleInputChannel = GraphBuilderVirtualSampleInputChannel;
  using VirtualSampleInputFamily = GraphBuilderVirtualSampleInputFamily;
  using VirtualSampleInputFamilies = GraphBuilderVirtualSampleInputFamilies;
  using VirtualSampleOutput = GraphBuilderVirtualSampleOutput;
  using VirtualEventOutput = GraphBuilderVirtualEventOutput;
  using VirtualOutputs = GraphBuilderVirtualOutputs;
  using VirtualSampleOutputChannel = GraphBuilderVirtualSampleOutputChannel;
  using VirtualSampleOutputFamily = GraphBuilderVirtualSampleOutputFamily;
  using VirtualSampleOutputFamilies = GraphBuilderVirtualSampleOutputFamilies;
  using VirtualPorts = GraphBuilderVirtualPorts;

  VacantInputs vacant_inputs() const;
  VirtualInputs virtual_inputs() const;
  VirtualSampleInputFamilies virtual_sample_input_families() const;
  VirtualOutputs virtual_outputs() const;
  VirtualSampleOutputFamilies virtual_sample_output_families() const;
  VirtualPorts virtual_ports() const;
  constexpr GraphBuilderPublicSamplePortFamilies public_sample_input_families() const;
  constexpr bool public_sample_input_is_connected(size_t port_ordinal) const;
  constexpr std::vector<GraphBuilderPublicEventInput> public_event_inputs() const;
  constexpr bool public_event_input_is_connected(size_t port_ordinal) const;
  constexpr std::span<SourceInfo const> public_event_input_source_infos(size_t) const;
  constexpr GraphBuilderPublicSamplePortFamilies public_sample_output_families() const;
  constexpr std::vector<GraphBuilderPublicEventOutput> public_event_outputs() const;

  constexpr void connect_sample_input(
      NodeBundlePortId target, SamplePortRef source);
  void connect_sample_input(NodeBundlePortId target, std::span<SamplePortRef const> sources);
  void connect_event_input(NodeBundlePortId target, EventPortRef source);
  bool sample_input_is_connected(NodeBundlePortId target) const;
  bool event_input_is_connected(NodeBundlePortId target) const;
  void connect_sample_output(NodeBundlePortId source, NodeRef const& target);
  EventPortRef event_output(NodeBundlePortId source) const;
  size_t sample_port_index(NodeBundleHandle, bool inputs, std::string_view name) const;
  size_t event_port_index(NodeBundleHandle, bool inputs, std::string_view name) const;
  consteval GraphIntrospectionMetadata build_metadata(size_t detach_id_offset = 0) const;
  consteval RootNodeBuildResult build_root_node(size_t detach_id_offset = 0) const;
  consteval RootNodeBuildResult build_execution_root_node(
      size_t detach_id_offset = 0) const;

private:
  constexpr SamplePortRef detach_sample_port(
      SamplePortRef const&, size_t loop_extra_latency);
  constexpr void record_authored_sample_connection(
      NodeBundlePortId, SamplePortRef const&);
  constexpr void record_authored_sample_connection(SampleInputChannelId, SamplePortRef const&);
  void record_authored_sample_connection(NodeBundlePortId, std::span<SamplePortRef const>);
  void record_authored_event_connection(NodeBundlePortId, EventPortRef const&);
  constexpr SamplePortRef lift_to_sample_port(
      SamplePortRef const& sample_port);
  constexpr SamplePortRef lift_to_sample_port(SamplePortRef&& sample_port);
  template<class T>
    requires(!std::same_as<std::remove_cvref_t<T>, SamplePortRef> &&
             requires(std::remove_cvref_t<T> const& ref) { ref.node_ref(); } &&
             std::convertible_to<T, SamplePortRef>)
  constexpr SamplePortRef lift_to_sample_port(T&& sample_port) {
    return lift_to_sample_port(static_cast<SamplePortRef>(std::forward<T>(sample_port)));
  }
  template<class ChannelType, SampleStreamLayout Layout>
  constexpr SamplePortRef lift_to_sample_port(TypedSamplePortRef<ChannelType, Layout> const& sample_port);
  template<class ChannelType, SampleStreamLayout Layout, class Member>
  constexpr SamplePortRef lift_to_sample_port(TypedSamplePortChannelRef<ChannelType, Layout, Member> const& sample_port);
  template<class ChannelType, SampleStreamLayout Layout>
  constexpr SamplePortRef lift_to_sample_port(TypedSamplePortTileRef<ChannelType, Layout> const& sample_port);
  template<class T>
    requires std::is_arithmetic_v<std::remove_cvref_t<T>> ||
             std::is_same_v<std::remove_cvref_t<T>, Sample>
  constexpr SamplePortRef lift_to_sample_port(T value) {
    auto constant = node<Constant>(static_cast<Sample>(value));
    return static_cast<SamplePortRef>(constant);
  }
  constexpr SamplePortRef lift_to_sample_port(NamedRef const& ref);
};

constexpr SubgraphBuilder::SubgraphBuilder(
    GraphBuilder& builder, NodeBundleHandle boundary)
    : _builder(builder), _ports(boundary) {}

constexpr PublicSampleInputRef SubgraphBuilder::input() {
  return input(Sample{0.0f});
}

constexpr PublicSampleInputRef SubgraphBuilder::input_named(
    std::string_view name, Sample value, std::optional<Sample> min,
    std::optional<Sample> max) {
  return PublicSampleInputRef(
      _ports.add_sample_input(_builder, _builder._node_bundles,
                              name, value, min, max));
}

constexpr PublicSampleInputRef SubgraphBuilder::input(
    Sample value, std::optional<Sample> min, std::optional<Sample> max) {
  return PublicSampleInputRef(
      _ports.add_sample_input(_builder, _builder._node_bundles,
                              {}, value, min, max));
}

constexpr PublicEventInputRef SubgraphBuilder::event_input_named(
    std::string_view name, EventTypeId type) {
  return PublicEventInputRef(
      _ports.add_event_input(_builder, _builder._node_bundles, name, type));
}

constexpr PublicEventInputRef SubgraphBuilder::event_input(EventTypeId type) {
  return PublicEventInputRef(
      _ports.add_event_input(_builder, _builder._node_bundles, {}, type));
}

constexpr void SubgraphBuilder::event_outputs(
    std::span<EventOutputRefConfig const> refs) {
  _ports.define_event_outputs(
      _builder, _builder._node_bundles, _builder._identity, refs);
}

constexpr void SubgraphBuilder::outputs(std::initializer_list<NamedRef> refs) {
  outputs(std::span<NamedRef const>(refs.begin(), refs.size()));
}

constexpr void SubgraphBuilder::outputs(std::span<OutputRefConfig const> refs) {
  _ports.define_sample_outputs(
      _builder, _builder._node_bundles, _builder._identity, refs);
}

constexpr void SubgraphBuilder::outputs(std::span<NamedRef const> refs) {
  _ports.define_sample_outputs_from_named_refs(
      _builder, _builder._node_bundles, _builder._identity,
      [&](auto&& value) {
        return _builder.lift_to_sample_port(
            std::forward<decltype(value)>(value));
      },
      refs);
}

constexpr void GraphBuilder::outputs(std::initializer_list<NamedRef> refs) {
  outputs(std::span<NamedRef const>(refs.begin(), refs.size()));
}

constexpr void GraphBuilder::outputs(std::span<NamedRef const> refs) {
  _public_ports.define_sample_outputs_from_named_refs(
      *this, _node_bundles, _identity,
      [&](auto&& value) {
        return lift_to_sample_port(std::forward<decltype(value)>(value));
      },
      refs);
}

constexpr void GraphBuilderPublicPorts::define_sample_outputs(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles,
    GraphBuilderIdentity const& identity, std::span<OutputRefConfig const> refs) {
  _last_sample_output_port_ordinals.clear();
  bool require_names = refs.size() > 1;
  for (size_t i = 0; i < refs.size(); ++i) {
    auto const& ref = refs[i].ref; auto const& config = refs[i].config;
    if (!ref.graph_builder) details::error("builder " + identity.value + ": outputs(...): empty SamplePortRef");
    if (ref.graph_builder != &builder) details::error("builder " + identity.value + ": outputs(...): SamplePortRef belongs to another builder");
    if (require_names && config.name.empty()) details::error("builder " + identity.value + ": outputs(...) requires names when exposing more than one sample output");
    auto existing = !refs[i].public_member.family_name.empty()
      ? std::find_if(_sample_output_members.begin(), _sample_output_members.end(), [&](auto const& m) {
          auto const& n = refs[i].public_member; return m.family_name == n.family_name && m.channel_type == n.channel_type && m.channel_index == n.channel_index && m.whole_stream == n.whole_stream; })
      : _sample_output_members.end();
    size_t output = existing == _sample_output_members.end()
      ? bundles.bundle(_boundary).boundary_sample_outputs().size()
      : static_cast<size_t>(existing - _sample_output_members.begin());
    if (existing == _sample_output_members.end()) {
      IV_ASSERT(bundles.bundle(_boundary).append_boundary_sample_output(config) == output, "public output metadata mismatch");
      _sample_output_members.push_back(refs[i].public_member); _sample_output_source_infos.emplace_back();
    }
    NodeBundlePortId const target{_boundary, PortKind::sample, output};
    if (refs[i].target_channel_ordinal) {
      auto channels = bundles.sample_input_channels(target); auto channel = *refs[i].target_channel_ordinal;
      if (channel >= channels.size()) details::error("public sample output channel ordinal is out of bounds");
      builder.record_authored_sample_connection(channels[channel], ref);
    } else builder.record_authored_sample_connection(target, ref);
    _last_sample_output_port_ordinals.push_back(output);
  }
  _sample_outputs_defined = true;
}

constexpr void GraphBuilderPublicPorts::define_event_outputs(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles,
    GraphBuilderIdentity const& identity,
    std::span<EventOutputRefConfig const> refs)
{
  auto& boundary = bundles.bundle(_boundary);
  boundary.clear_boundary_event_outputs();
  _event_output_source_infos.resize(refs.size());
  bool const require_names = refs.size() > 1;
  for (size_t i = 0; i < refs.size(); ++i) {
    auto const& ref = refs[i].ref;
    if (!ref.graph_builder)
      details::error(
          "builder " + identity.value +
          ": event_outputs(...): empty EventPortRef");
    if (ref.graph_builder != &builder)
      details::error(
          "builder " + identity.value +
          ": event_outputs(...): EventPortRef belongs to another builder");
    if (require_names && refs[i].config.name.empty())
      details::error(
          "builder " + identity.value +
          ": event_outputs(...) requires names when exposing more than one "
          "event output");
    auto config = refs[i].config;
    config.type = ref.type;
    auto output = boundary.append_boundary_event_output(std::move(config));
    builder.record_authored_event_connection(
        {_boundary, PortKind::event, output}, ref);
  }
}

template<class ChannelType, SampleStreamLayout Layout>
constexpr SamplePortRef GraphBuilder::lift_to_sample_port(TypedSamplePortRef<ChannelType, Layout> const& p) { return lift_to_sample_port(static_cast<SamplePortRef>(p)); }
template<class ChannelType, SampleStreamLayout Layout, class Member>
constexpr SamplePortRef GraphBuilder::lift_to_sample_port(TypedSamplePortChannelRef<ChannelType, Layout, Member> const& p) { return lift_to_sample_port(p.erased()); }
template<class ChannelType, SampleStreamLayout Layout>
constexpr SamplePortRef GraphBuilder::lift_to_sample_port(TypedSamplePortTileRef<ChannelType, Layout> const& p) { return lift_to_sample_port(p.erased()); }

constexpr GraphBuilder::GraphBuilder(GraphBuilderIdentity identity)
    : _identity(std::move(identity))
    , _public_ports(_node_bundles.append_boundary())
{}

constexpr GraphBuilder::GraphBuilder()
    : GraphBuilder(GraphBuilderIdentity("root"))
{}

constexpr PublicSampleInputRef GraphBuilder::input_named(
    std::string_view name, Sample value, std::optional<Sample> min,
    std::optional<Sample> max) {
  return PublicSampleInputRef(
      _public_ports.add_sample_input(*this, _node_bundles, name, value, min, max));
}

constexpr GraphBuilderPublicSamplePortFamilies
GraphBuilder::public_sample_input_families() const {
  return _public_ports.sample_input_families(_node_bundles);
}

constexpr bool GraphBuilder::public_sample_input_is_connected(size_t i) const {
  auto channels = _node_bundles.sample_output_channels(
      {_public_ports.boundary_handle(), PortKind::sample, i});
  return std::ranges::any_of(channels, [&](auto c) {
    return _connections.sample_output_is_connected(c);
  });
}

constexpr std::vector<GraphBuilderPublicEventInput>
GraphBuilder::public_event_inputs() const {
  return _public_ports.collected_event_inputs(_node_bundles);
}

constexpr bool GraphBuilder::public_event_input_is_connected(size_t i) const {
  auto ports = _node_bundles.event_output_ports(
      {_public_ports.boundary_handle(), PortKind::event, i});
  return std::ranges::any_of(ports, [&](auto p) {
    return _connections.event_output_is_connected(p);
  });
}

constexpr std::span<SourceInfo const>
GraphBuilder::public_event_input_source_infos(size_t i) const {
  return _public_ports.event_input_source_infos(i);
}

constexpr GraphBuilderPublicSamplePortFamilies
GraphBuilder::public_sample_output_families() const {
  return _public_ports.sample_output_families(_node_bundles);
}

constexpr std::vector<GraphBuilderPublicEventOutput>
GraphBuilder::public_event_outputs() const {
  return _public_ports.collected_event_outputs(_node_bundles);
}

consteval GraphIntrospectionMetadata GraphBuilder::build_metadata(
    size_t detach_offset) const
{
  auto lowered = GraphBuilderLowering::lower(
      _node_bundles, _connections, _public_ports, _virtual_nodes, _detach);
  auto metadata = GraphBuilderFinalizer::build_metadata(
      _identity, lowered, _node_bundles, _virtual_nodes, _connections,
      detach_offset);
  auto sample_inputs = public_sample_input_families();
  for (auto& family : sample_inputs.families) {
    family.authored_connected = std::ranges::any_of(
        family.channels, [&](auto const& channel) {
          return std::ranges::any_of(
              channel.port_ordinals, [&](auto ordinal) {
                return public_sample_input_is_connected(ordinal);
              });
        });
  }
  metadata.public_sample_inputs = std::move(sample_inputs.families);
  metadata.public_event_inputs = public_event_inputs();
  for (auto& input : metadata.public_event_inputs)
    input.graph_connected = public_event_input_is_connected(input.port_ordinal);
  metadata.public_sample_outputs = public_sample_output_families().families;
  metadata.public_event_outputs = public_event_outputs();
  return metadata;
}

consteval GraphBuilder::RootNodeBuildResult GraphBuilder::build_root_node(
    size_t detach_offset) const
{
  auto lowered=GraphBuilderLowering::lower(
      _node_bundles,_connections,_public_ports,_virtual_nodes,_detach);
  return GraphBuilderFinalizer::build_root_node(
      _identity,lowered,_node_bundles,_virtual_nodes,_public_ports,
      detach_offset);
}

consteval GraphBuilder::RootNodeBuildResult GraphBuilder::build_execution_root_node(
    size_t detach_offset) const
{
  auto lowered=GraphBuilderLowering::lower(
      _node_bundles,_connections,_public_ports,_virtual_nodes,_detach,
      true);
  return GraphBuilderFinalizer::build_root_node(
      _identity,lowered,_node_bundles,_virtual_nodes,_public_ports,
      detach_offset,true);
}

constexpr SamplePortRef::SamplePortRef(
    GraphBuilder& builder,
    NodeBundlePortId port)
    : graph_builder(&builder)
{
  if (port.port_kind != PortKind::sample)
    details::error(
        "attempted to create a SamplePortRef from an event NodeBundle port");
  channel_type = builder._node_bundles.resolve_sample_output(
      port).config.channel_layout.channel_type;
  channels = builder._node_bundles.sample_output_channels(port);
}

constexpr SamplePortRef GraphBuilder::lift_to_sample_port(
    SamplePortRef const& port)
{
  if (port.graph_builder != this)
    details::error("sample port belongs to another builder");
  return port;
}

constexpr SamplePortRef GraphBuilder::lift_to_sample_port(
    SamplePortRef&& port)
{
  if (port.graph_builder != this)
    details::error("sample port belongs to another builder");
  return std::move(port);
}

constexpr void GraphBuilder::record_authored_sample_connection(
    NodeBundlePortId target,
    SamplePortRef const& source)
{
  if (target.port_kind != PortKind::sample || !source.graph_builder
      || source.graph_builder != this)
    details::error("invalid authored sample connection");
  auto descriptor = _node_bundles.resolve_sample_input(target);
  _connections.record_authored_sample_connection({
      source.channel_type,
      source.channels,
      descriptor.config.channel_layout.channel_type,
      _node_bundles.sample_input_channels(target),
  });
}

constexpr void GraphBuilder::record_authored_sample_connection(
    SampleInputChannelId target,
    SamplePortRef const& source)
{
  if (!source.graph_builder || source.graph_builder != this)
    details::error("invalid authored sample channel connection");
  auto channels = _node_bundles.sample_input_channels(
      {target.bundle, PortKind::sample, target.port});
  if (target.channel >= channels.size() || channels[target.channel] != target)
    details::error("sample input channel does not belong to its NodeBundle port");
  _connections.record_authored_sample_connection({
      source.channel_type, source.channels, ChannelTypeId::mono, {target}});
}

constexpr void GraphBuilder::connect_sample_input(
    NodeBundlePortId target,
    SamplePortRef source)
{
  record_authored_sample_connection(target, source);
}

template<class Config>
constexpr void GraphBuilder::validate_output_port_configs(
    std::span<Config const> configs,
    std::string_view node_label,
    std::string_view kind) {
  GraphBuilderNodeBundles::validate_output_port_configs(configs, node_label, kind);
}

template<class Node, class... Args>
constexpr details::node_ref_for_t<Node> GraphBuilder::node(Args&&... args) {
  if consteval {
    using StoredNode = std::remove_cvref_t<Node>;
    StoredNode node_value(std::forward<Args>(args)...);
    auto concrete = GraphBuilderNodeBundles::make_concrete_node(
        details::reflect_node(node_value));
    auto const handle = _node_bundles.append_concrete(std::move(concrete));
    if constexpr (details::should_preserve_node_type_v<StoredNode>) {
      return TypedNodeRef<StoredNode>(*this, handle);
    } else {
      return NodeRef(*this, handle);
    }
  } else {
    details::runtime_graph_builder_node_call_is_forbidden();
    return {};
  }
}

template<class Node, class ChannelType, class... Args>
constexpr auto GraphBuilder::node(Args&&... args) {
  if consteval {
    using StoredNode = std::remove_cvref_t<Node>;
    static_assert((std::copy_constructible<std::remove_cvref_t<Args>> && ...),
        "tiled-node construction requires reusable constructor arguments");

    StoredNode node_value(args...);
    auto concrete = GraphBuilderNodeBundles::make_concrete_node(
        details::reflect_node(node_value));
    for (auto const& config : concrete.inputs()) {
      if (config.channel_layout.channel_type != ChannelTypeId::mono) {
        details::error(
            "the tiled-node model requires fully mono concrete sample nodes");
      }
    }
    for (auto const& config : concrete.outputs()) {
      if (config.channel_layout.channel_type != ChannelTypeId::mono) {
        details::error(
            "the tiled-node model requires fully mono concrete sample nodes");
      }
    }

    std::array<NodeBundleHandle, ChannelType::channel_count> members {};
    for (size_t channel = 0; channel < ChannelType::channel_count; ++channel) {
      members[channel] = _node_bundles.append_concrete(concrete);
    }
    auto const handle = _node_bundles.append_tiled(
        members,
        ChannelLayout{
            .channel_type = ChannelTypeTraits<ChannelType>::id,
            .sample_layout = SampleStreamLayout::planar,
        });
    return TiledNodeRef<StoredNode, ChannelType>(*this, handle);
  } else {
    details::runtime_graph_builder_node_call_is_forbidden();
    return TiledNodeRef<
        std::remove_cvref_t<Node>,
        ChannelType> {};
  }
}

constexpr SamplePortRef SamplePortRef::detach(size_t latency) const {
  if (!graph_builder)
    details::error("attempted to detach an empty sample port");
  return graph_builder->detach_sample_port(*this, latency);
}

constexpr SamplePortRef GraphBuilder::detach_sample_port(
    SamplePortRef const& source, size_t latency) {
  if (!source.graph_builder || source.graph_builder != this)
    details::error("cannot detach a sample port from another builder");
  if (source.channels.empty())
    details::error("cannot detach a sample port with no semantic channels");
  if (_detach.reader_output_exists(source.channel_type, source.channels))
    return source;
  if (auto existing = _detach.info_for_source(source.channel_type, source.channels)) {
    if (existing->loop_extra_latency != latency)
      details::error("detach loop extra latency conflict");
    return SamplePortRef(
        *this, {existing->reader_bundle, PortKind::sample, 0});
  }
  if (latency < 1)
    details::error("detach loop extra latency must be at least 1");
  auto id = _detach.allocate_detach_id();
  auto writer = node<DetachWriterNode>(id, latency);
  record_authored_sample_connection(
      {writer.node_bundle_handle(), PortKind::sample, 0}, source);
  auto reader = node<DetachReaderNode>(id, latency);
  SamplePortRef detached = static_cast<SamplePortRef>(reader);
  if (detached.channel_type != ChannelTypeId::mono ||
      detached.channels.size() != 1)
    details::error("detach reader must expose exactly one mono sample channel");
  _detach.record_detached_source({
      .detach_id = id,
      .source_type = source.channel_type,
      .source_channels = source.channels,
      .writer_bundle = writer.node_bundle_handle(),
      .reader_bundle = reader.node_bundle_handle(),
      .reader_channel = detached.channels.front(),
      .loop_extra_latency = latency,
  });
  return detached;
}

constexpr SamplePortRef GraphBuilder::lift_to_sample_port(
    NamedRef const& ref) {
  return std::visit(
      [&](auto const& value) -> SamplePortRef {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<T, EventPortRef>)
          details::error("expected sample value, got event");
        else
          return lift_to_sample_port(value);
      },
      ref.value);
}

template<class ChannelType, class... Refs>
constexpr auto GraphBuilder::tile(Refs&&... refs) {
  static_assert(sizeof...(Refs) == ChannelType::channel_count,
      "g.tile<ChannelType>(...) requires exactly one source per channel");
  std::array<SamplePortRef, ChannelType::channel_count> members{
      lift_to_sample_port(std::forward<Refs>(refs))...};
  for (auto const& member : members)
    if (member.channel_type != ChannelTypeId::mono || member.channels.size()!=1)
      details::error("g.tile<ChannelType>(...) requires scalar sample sources");
  return TypedSamplePortTileRef<ChannelType>{std::move(members)};
}

template<class... Refs>
void GraphBuilder::event_outputs(Refs&&... refs) {
  _public_ports.define_event_outputs_from_args(*this, _node_bundles, _identity,
                                               std::forward<Refs>(refs)...);
}

template<auto Module>
NodeRef GraphBuilder::module(std::string_view kind) {
  static_assert(std::invocable<decltype(Module), GraphBuilder&>,
      "iv::GraphBuilder::module<Module>() requires Module(GraphBuilder&)");
  static_assert(std::same_as<std::invoke_result_t<decltype(Module), GraphBuilder&>, void>,
      "iv::GraphBuilder::module<Module>() requires Module(GraphBuilder&) to return void");

  auto child = derive_nested_builder();
  std::invoke(Module, child);
  return embed_subgraph(child, kind);
}

template<class Fn>
NodeRef GraphBuilder::subgraph(Fn&& fn, std::string_view kind) {
  auto const boundary = _node_bundles.append_scope_boundary();
  SubgraphBuilder subgraph_builder(*this, boundary);
  auto const child_begin = _node_bundles.size();

  if constexpr (std::invocable<decltype(std::forward<Fn>(fn)), SubgraphBuilder&>)
  {
    std::invoke(std::forward<Fn>(fn), subgraph_builder);
  }
  else if constexpr (std::invocable<decltype(std::forward<Fn>(fn))>)
  {
    std::invoke(std::forward<Fn>(fn));
  }
  else
  {
    static_assert(false, "iv::GraphBuilder::subgraph(Fn) requires a callback with no arguments or accepting iv::SubgraphBuilder&");
  }

  auto const child_count = _node_bundles.size() - child_begin;
  NodeRef result(
      *this,
      _node_bundles.append_subgraph(
          boundary, child_begin, child_count, kind));
  for (auto const& info :
       _node_bundles.bundle(boundary).source_annotations().infos) {
    result._annotate_source_info(
        info.declaration_identity, info.span.file_path,
        info.span.begin, info.span.end);
  }
  return result;
}

template<class... Refs>
constexpr void GraphBuilder::outputs(Refs&&... refs) {
  _public_ports.define_sample_outputs_from_args(*this, _node_bundles, _identity,
      [&](auto&& value){ return lift_to_sample_port(std::forward<decltype(value)>(value)); },
      std::forward<Refs>(refs)...);
}

template<class... Refs>
void SubgraphBuilder::event_outputs(Refs&&... refs) {
  _ports.define_event_outputs_from_args(
      _builder, _builder._node_bundles, _builder._identity,
      std::forward<Refs>(refs)...);
}

template<class... Refs>
void SubgraphBuilder::outputs(Refs&&... refs) {
  _ports.define_sample_outputs_from_args(
      _builder, _builder._node_bundles, _builder._identity,
      [&](auto&& value) {
        return _builder.lift_to_sample_port(
            std::forward<decltype(value)>(value));
      },
      std::forward<Refs>(refs)...);
}

// NodeRef and typed-ref definitions are semantic: every operation addresses a
// NodeBundle and no authored ref can be projected to a topology address.
template<class Node, class PortProjection>
constexpr NodePorts const& TypedNodeRef<Node, PortProjection>::ports() const {
  if (!_graph_builder) details::error("attempted to use a null NodeRef");
  return _graph_builder->_node_bundles.typed_ports(_index);
}
constexpr NodeRef NodeRef::node_ref() const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); return NodeRef(*_graph_builder,_index); }
constexpr NodeRef NodeRef::_clone_handle() const { return _graph_builder?NodeRef(*_graph_builder,_index):NodeRef{}; }
constexpr SamplePortRef NodeRef::operator[](size_t output_port) const {
  if(!_graph_builder)details::error("attempted to use a null NodeRef");
  if(output_port>=_graph_builder->_node_bundles.bundle(_index).sample_output_count())details::error("sample output port is out of bounds on "+to_string());
  return SamplePortRef(*_graph_builder,{_index,PortKind::sample,output_port});
}
constexpr SamplePortRef NodeRef::operator[](std::string_view name) const { return (*this)[_graph_builder->sample_port_index(_index,false,name)]; }
constexpr NodeRef::operator SamplePortRef() const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); if(sample_output_count()!=1)details::error(to_string()+" does not have exactly 1 output port"); return (*this)[0]; }
constexpr size_t NodeRef::sample_input_count() const { return _graph_builder->_node_bundles.bundle(node_bundle_handle()).sample_input_count(); }
constexpr size_t NodeRef::sample_output_count() const { return _graph_builder->_node_bundles.bundle(node_bundle_handle()).sample_output_count(); }
constexpr size_t NodeRef::event_input_count() const { return _graph_builder->_node_bundles.bundle(node_bundle_handle()).event_input_count(); }
constexpr size_t NodeRef::event_output_count() const { return _graph_builder->_node_bundles.bundle(node_bundle_handle()).event_output_count(); }
constexpr bool NodeRef::input_is_connected(size_t i) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); return _graph_builder->sample_input_is_connected({_index,PortKind::sample,i}); }
constexpr bool NodeRef::event_input_is_connected(size_t i) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); return _graph_builder->event_input_is_connected({_index,PortKind::event,i}); }
template<class T> constexpr NodeRef NodeRef::connect_input(size_t i,T&& value) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); auto source=_graph_builder->lift_to_sample_port(std::forward<T>(value)); if(source.graph_builder!=_graph_builder)details::error("sample source belongs to another builder"); _graph_builder->connect_sample_input({_index,PortKind::sample,i},std::move(source)); return _clone_handle(); }
template<class T> constexpr NodeRef NodeRef::connect_input(std::string_view n,T&& v) const { return connect_input(_graph_builder->sample_port_index(_index,true,n),std::forward<T>(v)); }
template<class... Args> constexpr NodeRef NodeRef::operator()(Args&&... args) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); size_t ps=0,pe=0; auto connect=[&](auto&& arg){using A=std::remove_cvref_t<decltype(arg)>; if constexpr(details::is_named_arg_v<A>){if constexpr(A::kind==NamedPortKind::sample)connect_input(_graph_builder->sample_port_index(_index,true,A::name.view()),std::forward<decltype(arg)>(arg).value);else connect_event_input(_graph_builder->event_port_index(_index,true,A::name.view()),static_cast<EventPortRef>(std::forward<decltype(arg)>(arg).value));}else if constexpr(std::convertible_to<A,EventPortRef>)connect_event_input(pe++,static_cast<EventPortRef>(std::forward<decltype(arg)>(arg)));else connect_input(ps++,std::forward<decltype(arg)>(arg));};(connect(std::forward<Args>(args)),...);return _clone_handle(); }
constexpr NodeRef NodeRef::connect_event_input(size_t i,EventPortRef value) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); if(value.graph_builder!=_graph_builder)details::error("event source belongs to another builder"); _graph_builder->connect_event_input({_index,PortKind::event,i},std::move(value)); return _clone_handle(); }
constexpr NodeRef NodeRef::connect_event_input(std::string_view n,EventPortRef v) const { return connect_event_input(_graph_builder->event_port_index(_index,true,n),std::move(v)); }
constexpr EventPortRef NodeRef::event_port(size_t i) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); return _graph_builder->event_output({_index,PortKind::event,i}); }
constexpr EventPortRef NodeRef::event_port(std::string_view n) const { return event_port(_graph_builder->event_port_index(_index,false,n)); }
constexpr EventPortRef NodeRef::event_port() const { if(event_output_count()!=1)details::error(to_string()+" does not have exactly 1 event output port"); return event_port(0); }
constexpr NodeRef NodeRef::ttl(size_t ttl_samples) const { if(!_graph_builder)details::error("attempted to use a null NodeRef"); _graph_builder->_node_bundles.apply_ttl(_index,ttl_samples); return _clone_handle(); }
constexpr NodeRef NodeRef::no_ttl() const { return ttl(std::numeric_limits<size_t>::max()); }
inline std::string NodeRef::to_string() const { return _graph_builder?"node bundle "+std::to_string(_index):"empty node"; }

inline NodeRef& NodeRef::operator=(NodeRef const& rhs) {
  if(this==&rhs)return *this;
  if(_allows_single_assignment){if(!rhs._graph_builder)details::error("cannot initialize a virtual-empty NodeRef from an empty NodeRef");_graph_builder=rhs._graph_builder;_index=rhs._index;if(!_virtual_declaration_id.empty())_graph_builder->_annotations.attach_virtual_node(_graph_builder->_node_bundles,_graph_builder->_virtual_nodes,_index,_virtual_declaration_id);_allows_single_assignment=false;return *this;}
  if(_graph_builder||!_virtual_declaration_id.empty())details::error(
    "cannot assign to NodeRef '" + _virtual_declaration_id +
    "' after it has already been initialized");
  _graph_builder=rhs._graph_builder;_index=rhs._index;_virtual_declaration_id=rhs._virtual_declaration_id;_allows_single_assignment=rhs._allows_single_assignment;return *this;
}
inline NodeRef& NodeRef::operator=(NodeRef&& rhs) {
  if(this==&rhs)return *this;
  if(_allows_single_assignment){if(!rhs._graph_builder)details::error("cannot initialize a virtual-empty NodeRef from an empty NodeRef");_graph_builder=rhs._graph_builder;_index=rhs._index;if(!_virtual_declaration_id.empty())_graph_builder->_annotations.attach_virtual_node(_graph_builder->_node_bundles,_graph_builder->_virtual_nodes,_index,_virtual_declaration_id);_allows_single_assignment=false;rhs._graph_builder=nullptr;rhs._index=0;rhs._virtual_declaration_id.clear();rhs._allows_single_assignment=false;return *this;}
  if(_graph_builder||!_virtual_declaration_id.empty())details::error(
    "cannot assign to NodeRef '" + _virtual_declaration_id +
    "' after it has already been initialized");
  _graph_builder=rhs._graph_builder;_index=rhs._index;_virtual_declaration_id=std::move(rhs._virtual_declaration_id);_allows_single_assignment=rhs._allows_single_assignment;rhs._graph_builder=nullptr;rhs._index=0;rhs._virtual_declaration_id.clear();rhs._allows_single_assignment=false;return *this;
}
constexpr void NodeRef::_annotate_source_info(std::string_view declaration_identity,std::string_view file_path,uint32_t begin,uint32_t end) const {
  if(!declaration_identity.empty())_virtual_declaration_id=declaration_identity;
  if(!_graph_builder)return;
  _graph_builder->_annotations.annotate_node_source_info(_graph_builder->_node_bundles,_graph_builder->_virtual_nodes,_graph_builder->_identity,_index,declaration_identity,file_path,begin,end);
}

template<class Node,class PortProjection> constexpr SamplePortRef TypedNodeRef<Node,PortProjection>::operator[](size_t i) const { return NodeRef::operator[](i); }
template<class Node,class PortProjection> constexpr SamplePortRef TypedNodeRef<Node,PortProjection>::operator[](std::string_view n) const { if(!_graph_builder)details::error("attempted to use a null NodeRef");auto outputs=get_outputs(ports());std::optional<size_t> match;for(size_t i=0;i<outputs.size();++i)if(outputs[i].name==n){if(match)details::error("output name is ambiguous");match=i;}if(match)return NodeRef::operator[](*match);details::error("output port does not exist on "+to_string()); }
template<class Node,class PortProjection> constexpr EventPortRef TypedNodeRef<Node,PortProjection>::event_port(size_t i) const { if(i>=ports().event_outputs().size())details::error("event output port out of bounds");return NodeRef::event_port(i); }
template<class Node,class PortProjection> constexpr EventPortRef TypedNodeRef<Node,PortProjection>::event_port(std::string_view n) const { auto const& outputs=ports().event_outputs();std::optional<size_t> match;for(size_t i=0;i<outputs.size();++i)if(outputs[i].name==n){if(match)details::error("event output name is ambiguous");match=i;}if(match)return NodeRef::event_port(*match);details::error("event output port does not exist on "+to_string()); }
template<class Node,class PortProjection> constexpr EventPortRef TypedNodeRef<Node,PortProjection>::event_port() const { if(ports().event_outputs().size()!=1)details::error(to_string()+" does not have exactly 1 event output port");return event_port(0); }
template<class Node,class PortProjection> constexpr TypedNodeRef<Node,PortProjection>::operator SamplePortRef() const { if(get_num_outputs(ports())!=1)details::error(to_string()+" does not have exactly 1 output port");return SamplePortRef(*_graph_builder,{_index,PortKind::sample,0}); }

template <class Node, class PortProjection>
template <class... Args>
    requires(details::node_call_enabled<std::remove_cvref_t<Node>, Args...>)
constexpr TypedNodeRef<Node, PortProjection>
TypedNodeRef<Node, PortProjection>::operator()(Args&&... args) const
{
    if (!this->_graph_builder)
        details::error("attempted to use a null NodeRef");

    auto inputs = get_inputs(ports());
    auto const& events = ports().event_inputs();
    auto sample = [&](SamplePortRef const& ref, size_t i) {
        if (i >= inputs.size())
            details::error("too many sample inputs");
        if (ref.graph_builder != this->_graph_builder)
            details::error("sample source belongs to another builder");
        this->_graph_builder->connect_sample_input(
            { this->_index, PortKind::sample, i },
            ref
        );
    };
    auto event = [&](EventPortRef ref, size_t i) {
        if (i >= events.size())
            details::error("too many event inputs");
        if (ref.graph_builder != this->_graph_builder)
            details::error("event source belongs to another builder");
        this->_graph_builder->connect_event_input(
            { this->_index, PortKind::event, i },
            ref
        );
    };

    size_t ps = 0;
    size_t pe = 0;
    auto process = [&](auto&& arg) {
        using A = std::remove_cvref_t<decltype(arg)>;
        if constexpr (details::is_named_arg_v<A>) {
            if constexpr (A::kind == NamedPortKind::event) {
                for (size_t i = 0; i < events.size(); ++i) {
                    if (events[i].name == A::name.view()) {
                        event(lift_event_operand(arg.value), i);
                        return;
                    }
                }
                details::error("named event input does not exist");
            } else {
                for (size_t i = 0; i < inputs.size(); ++i) {
                    if (inputs[i].name == A::name.view()) {
                        sample(this->_graph_builder->lift_to_sample_port(arg.value), i);
                        return;
                    }
                }
                details::error("named sample input does not exist");
            }
        } else if constexpr (details::graph_builder_event_port_like<decltype(arg)>) {
            event(static_cast<EventPortRef>(std::forward<decltype(arg)>(arg)), pe++);
        } else {
            sample(
                this->_graph_builder->lift_to_sample_port(
                    std::forward<decltype(arg)>(arg)
                ),
                ps++
            );
        }
    };
    (process(std::forward<Args>(args)), ...);
    return this->_clone_handle();
}
template<class Node,class PortProjection> template<class T> constexpr TypedNodeRef<Node,PortProjection> TypedNodeRef<Node,PortProjection>::connect_input(size_t i,T&& value) const {auto inputs=get_inputs(ports());if(i>=inputs.size())details::error("sample input out of bounds");auto ref=_graph_builder->lift_to_sample_port(std::forward<T>(value));_graph_builder->connect_sample_input({_index,PortKind::sample,i},ref);return this->_clone_handle();}
template<class Node,class PortProjection> template<class T> constexpr TypedNodeRef<Node,PortProjection> TypedNodeRef<Node,PortProjection>::connect_input(std::string_view n,T&& value) const {auto inputs=get_inputs(ports());std::optional<size_t> m;for(size_t i=0;i<inputs.size();++i)if(inputs[i].name==n){if(m)details::error("input name is ambiguous");m=i;}if(m)return connect_input(*m,std::forward<T>(value));details::error("input port does not exist");}
template<class Node,class PortProjection> constexpr TypedNodeRef<Node,PortProjection> TypedNodeRef<Node,PortProjection>::connect_event_input(size_t i,EventPortRef value) const {if(i>=ports().event_inputs().size())details::error("event input out of bounds");_graph_builder->connect_event_input({_index,PortKind::event,i},value);return this->_clone_handle();}
template<class Node,class PortProjection> constexpr TypedNodeRef<Node,PortProjection> TypedNodeRef<Node,PortProjection>::connect_event_input(std::string_view n,EventPortRef value) const {auto const& inputs=ports().event_inputs();std::optional<size_t> m;for(size_t i=0;i<inputs.size();++i)if(inputs[i].name==n){if(m)details::error("event input name is ambiguous");m=i;}if(m)return connect_event_input(*m,value);details::error("event input port does not exist");}
template<class Node,class PortProjection> constexpr SamplePortRef TypedNodeRef<Node,PortProjection>::detach(size_t latency) const {return static_cast<SamplePortRef>(*this).detach(latency);}
} // namespace iv
