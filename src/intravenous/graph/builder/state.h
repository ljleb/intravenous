#pragma once

// Private implementation for libiv_builder only. Do not include from a
// module TU.
#include <intravenous/graph/builder.h>
#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_nodes/type_erased.h>
#include <intravenous/channel_ports.h>
#include <intravenous/graph/builder/annotations.hpp>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/detach.hpp>
#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_refs.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/reflected_node_description.h>
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
class GraphBuilderState;
namespace details {
struct SubgraphBuildScope {
  GraphBuilderState* owner;
  GraphBuilderPublicPorts ports;
  size_t child_begin;
  bool finished = false;

  SubgraphBuildScope(
      GraphBuilderState& owner_, NodeBundleHandle boundary, size_t child_begin_)
      : owner(&owner_), ports(boundary), child_begin(child_begin_) {}
};
} // namespace details

class GraphBuilderState {
  friend class GraphBuilder;
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

  GraphBuilderIdentity _identity;
  GraphBuilderNodeBundles _node_bundles;
  GraphBuilderConnections _connections;
  GraphBuilderPublicPorts _public_ports;
  GraphBuilderDetach _detach;
  GraphBuilderAnnotations _annotations;
  GraphBuilderVirtualNodes _virtual_nodes;
  struct SamplePortExpression {
    ChannelTypeId channel_type = ChannelTypeId::mono;
    std::vector<SampleOutputChannelId> channels{};
  };
  struct EventPortExpression {
    EventTypeId type = EventTypeId::empty;
    std::vector<EventOutputPortId> sources{};
  };
  // SamplePortRef intentionally stores only an index into this session-owned
  // table. Variable-length expressions never live in module-generated code.
  std::vector<SamplePortExpression> _sample_port_expressions;
  std::vector<EventPortExpression> _event_port_expressions;
  // Ref values must point at the module-facing façade that issued them.  A
  // state object never escapes the session, so this is set only while a
  // facade is actively building into the session.
  GraphBuilder* _facade = nullptr;

  GraphBuilder& facade() const {
    if (!_facade) details::error("builder state has no active GraphBuilder facade");
    return *_facade;
  }

  constexpr explicit GraphBuilderState(GraphBuilderIdentity identity);
  NodeRef embed_subgraph(GraphBuilderState const& child, std::string_view kind = "Subgraph");
  constexpr PublicSampleInputRef input_named(std::string_view name, Sample default_value,
                                   std::optional<Sample> min,
                                   std::optional<Sample> max);
  constexpr PublicSampleInputRef input_named(
      std::string_view name, ChannelLayout channel_layout,
      Sample default_value, std::optional<Sample> min,
      std::optional<Sample> max);
  PublicEventInputRef event_input_named(std::string_view name, EventTypeId type);

public:
  void bind(GraphBuilder& builder) noexcept { _facade = &builder; }
  constexpr GraphBuilderState();
  NodeBundleHandle append_node_description(ReflectedNodeDescription);
  NodeBundleHandle append_tiled_node_description(
      ReflectedNodeDescription const&, ChannelLayout);
  PublicSampleInputRef input();
  PublicSampleInputRef input(Sample default_value,
                             std::optional<Sample> min = std::nullopt,
                             std::optional<Sample> max = std::nullopt);
  PublicEventInputRef event_input(EventTypeId type);

  details::SubgraphBuildScope* begin_subgraph();
  NodeRef finish_subgraph(
      details::SubgraphBuildScope&, std::string_view kind);
  void abandon_subgraph(details::SubgraphBuildScope*) noexcept;
  PublicSampleInputRef subgraph_input(
      details::SubgraphBuildScope&, std::string_view, ChannelLayout,
      Sample, std::optional<Sample>, std::optional<Sample>);
  PublicEventInputRef subgraph_event_input(
      details::SubgraphBuildScope&, std::string_view, EventTypeId);
  void subgraph_outputs(
      details::SubgraphBuildScope&, std::span<SampleOutputRequest const>);
  void subgraph_outputs(
      details::SubgraphBuildScope&, std::span<NamedRef const>);
  void subgraph_event_outputs(
      details::SubgraphBuildScope&, std::span<EventOutputRequest const>);

  void annotate_public_sample_input_source_info(PublicSampleInputRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_public_event_input_source_info(PublicEventInputRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_sample_port_source_info(SamplePortRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_event_port_source_info(EventPortRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_public_sample_output_source_info(std::span<SourceInfo const> infos);
  void annotate_public_event_output_source_info(std::span<SourceInfo const> infos);
  void annotate_public_sample_output_source_info(
      size_t ordinal, SourceInfo info);
  void annotate_public_event_output_source_info(
      size_t ordinal, SourceInfo info);

  void event_outputs(std::span<EventOutputRequest const> refs);
  constexpr void outputs(std::initializer_list<NamedRef> refs);
  void outputs(std::span<SampleOutputRequest const> refs);
  constexpr void outputs(std::span<NamedRef const> refs);

  using VacantSampleInput = GraphBuilderVacantSampleInput;
  using VacantEventInput = GraphBuilderVacantEventInput;
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
  size_t sample_input_count(NodeBundleHandle) const;
  size_t sample_output_count(NodeBundleHandle) const;
  size_t event_input_count(NodeBundleHandle) const;
  size_t event_output_count(NodeBundleHandle) const;
  InputConfig sample_input_config(NodeBundleHandle, size_t) const;
  EventInputConfig event_input_config(NodeBundleHandle, size_t) const;
  NodeBundleHandle tiled_member(NodeBundleHandle, size_t) const;
  NodePorts const& typed_ports(NodeBundleHandle) const;
  SamplePortRef sample_port_from_output(NodeBundlePortId);
  EventPortRef event_port_from_output(NodeBundlePortId) const;
  void apply_ttl(NodeBundleHandle, size_t);
  void annotate_node(NodeBundleHandle, std::string_view, std::string_view,
      uint32_t, uint32_t);
  constexpr ConfiguredGraph finish() const &;
  constexpr ConfiguredGraph finish() &&;

private:
  constexpr SamplePortRef detach_sample_port(
      SamplePortRef const&, size_t loop_extra_latency);
  SamplePortRef make_sample_port(
      ChannelTypeId, std::span<SampleOutputChannelId const>);
  std::span<SampleOutputChannelId const> sample_port_channels(
      SamplePortRef const&) const;
  EventPortRef make_event_port(
      EventTypeId, std::span<EventOutputPortId const>);
  std::span<EventOutputPortId const> event_port_sources(
      EventPortRef const&) const;
  constexpr void record_configured_sample_connection(
      NodeBundlePortId, SamplePortRef const&);
  constexpr void record_configured_sample_connection(SampleInputChannelId, SamplePortRef const&);
  void record_configured_sample_connection(NodeBundlePortId, std::span<SamplePortRef const>);
  void record_configured_event_connection(NodeBundlePortId, EventPortRef const&);
  constexpr SamplePortRef lift_to_sample_port(
      SamplePortRef const& sample_port);
  constexpr SamplePortRef lift_to_sample_port(SamplePortRef&& sample_port);
  constexpr void populate_public_introspection_metadata(
      GraphIntrospectionMetadata& metadata) const;
  constexpr SamplePortRef lift_to_sample_port(NamedRef const& ref);
  details::SubgraphBuildScope& require_subgraph_scope(
      details::SubgraphBuildScope&);
};

constexpr void GraphBuilderState::outputs(std::initializer_list<NamedRef> refs) {
  outputs(std::span<NamedRef const>(refs.begin(), refs.size()));
}

constexpr void GraphBuilderState::outputs(std::span<NamedRef const> refs) {
  _public_ports.define_sample_outputs_from_named_refs(
      *this, _node_bundles, _identity,
      [&](auto&& value) {
        return lift_to_sample_port(std::forward<decltype(value)>(value));
      },
      refs);
}

constexpr void GraphBuilderPublicPorts::define_sample_outputs(
    GraphBuilderState& builder, GraphBuilderNodeBundles& bundles,
    GraphBuilderIdentity const& identity, std::span<OutputRefConfig const> refs) {
  _last_sample_output_port_ordinals.clear();
  bool require_names = refs.size() > 1;
  for (size_t i = 0; i < refs.size(); ++i) {
    auto const& ref = refs[i].ref; auto const& config = refs[i].config;
    if (!ref.graph_builder) details::error("builder " + identity.value + ": outputs(...): empty SamplePortRef");
    if (ref.graph_builder != &builder.facade()) details::error("builder " + identity.value + ": outputs(...): SamplePortRef belongs to another builder");
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
      builder.record_configured_sample_connection(channels[channel], ref);
    } else builder.record_configured_sample_connection(target, ref);
    _last_sample_output_port_ordinals.push_back(output);
  }
  _sample_outputs_defined = true;
}

constexpr void GraphBuilderPublicPorts::define_event_outputs(
    GraphBuilderState& builder, GraphBuilderNodeBundles& bundles,
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
    if (ref.graph_builder != &builder.facade())
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
    builder.record_configured_event_connection(
        {_boundary, PortKind::event, output}, ref);
  }
}

constexpr GraphBuilderState::GraphBuilderState(GraphBuilderIdentity identity)
    : _identity(std::move(identity))
    , _public_ports(_node_bundles.append_boundary())
{}

constexpr GraphBuilderState::GraphBuilderState()
    : GraphBuilderState(GraphBuilderIdentity("root"))
{}

constexpr PublicSampleInputRef GraphBuilderState::input_named(
    std::string_view name, Sample value, std::optional<Sample> min,
    std::optional<Sample> max) {
  return PublicSampleInputRef(
      _public_ports.add_sample_input(*this, _node_bundles, name, value, min, max));
}

constexpr PublicSampleInputRef GraphBuilderState::input_named(
    std::string_view name, ChannelLayout channel_layout, Sample value,
    std::optional<Sample> min, std::optional<Sample> max) {
  return PublicSampleInputRef(_public_ports.add_sample_input(
      *this, _node_bundles, name, channel_layout, value, min, max));
}

constexpr GraphBuilderPublicSamplePortFamilies
GraphBuilderState::public_sample_input_families() const {
  return _public_ports.sample_input_families(_node_bundles);
}

constexpr bool GraphBuilderState::public_sample_input_is_connected(size_t i) const {
  auto channels = _node_bundles.sample_output_channels(
      {_public_ports.boundary_handle(), PortKind::sample, i});
  return std::ranges::any_of(channels, [&](auto c) {
    return _connections.sample_output_is_connected(c);
  });
}

constexpr std::vector<GraphBuilderPublicEventInput>
GraphBuilderState::public_event_inputs() const {
  return _public_ports.collected_event_inputs(_node_bundles);
}

constexpr bool GraphBuilderState::public_event_input_is_connected(size_t i) const {
  auto ports = _node_bundles.event_output_ports(
      {_public_ports.boundary_handle(), PortKind::event, i});
  return std::ranges::any_of(ports, [&](auto p) {
    return _connections.event_output_is_connected(p);
  });
}

constexpr std::span<SourceInfo const>
GraphBuilderState::public_event_input_source_infos(size_t i) const {
  return _public_ports.event_input_source_infos(i);
}

constexpr GraphBuilderPublicSamplePortFamilies
GraphBuilderState::public_sample_output_families() const {
  return _public_ports.sample_output_families(_node_bundles);
}

constexpr std::vector<GraphBuilderPublicEventOutput>
GraphBuilderState::public_event_outputs() const {
  return _public_ports.collected_event_outputs(_node_bundles);
}

constexpr void GraphBuilderState::populate_public_introspection_metadata(
    GraphIntrospectionMetadata& metadata) const
{
  auto sample_inputs = public_sample_input_families();
  for (auto& family : sample_inputs.families) {
    family.configured_connected = std::ranges::any_of(
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
}

constexpr ConfiguredGraph GraphBuilderState::finish() const & {
  auto bundles = _node_bundles;
  bundles.materialize_deferred_detaches();
  return {
      .identity = _identity,
      .node_bundles = std::move(bundles),
      .connections = _connections,
      .public_ports = _public_ports,
      .detach = _detach,
      .annotations = _annotations,
      .virtual_nodes = _virtual_nodes,
  };
}

constexpr ConfiguredGraph GraphBuilderState::finish() && {
  _node_bundles.materialize_deferred_detaches();
  return {
      .identity = std::move(_identity),
      .node_bundles = std::move(_node_bundles),
      .connections = std::move(_connections),
      .public_ports = std::move(_public_ports),
      .detach = std::move(_detach),
      .annotations = std::move(_annotations),
      .virtual_nodes = std::move(_virtual_nodes),
  };
}

constexpr SamplePortRef GraphBuilderState::lift_to_sample_port(
    SamplePortRef const& port)
{
  if (port.graph_builder != &facade())
    details::error("sample port belongs to another builder");
  return port;
}

constexpr SamplePortRef GraphBuilderState::lift_to_sample_port(
    SamplePortRef&& port)
{
  if (port.graph_builder != &facade())
    details::error("sample port belongs to another builder");
  return std::move(port);
}

constexpr void GraphBuilderState::record_configured_sample_connection(
    NodeBundlePortId target,
    SamplePortRef const& source)
{
  if (target.port_kind != PortKind::sample || !source.graph_builder
      || source.graph_builder != &facade())
    details::error("invalid configured sample connection");
  auto descriptor = _node_bundles.resolve_sample_input(target);
  _connections.record_configured_sample_connection({
      source.channel_type,
      {source.channels().begin(), source.channels().end()},
      descriptor.config.channel_layout.channel_type,
      _node_bundles.sample_input_channels(target),
  });
}

constexpr void GraphBuilderState::record_configured_sample_connection(
    SampleInputChannelId target,
    SamplePortRef const& source)
{
  if (!source.graph_builder || source.graph_builder != &facade())
    details::error("invalid configured sample channel connection");
  auto channels = _node_bundles.sample_input_channels(
      {target.bundle, PortKind::sample, target.port});
  if (target.channel >= channels.size() || channels[target.channel] != target)
    details::error("sample input channel does not belong to its NodeBundle port");
  _connections.record_configured_sample_connection({
      source.channel_type,
      {source.channels().begin(), source.channels().end()},
      ChannelTypeId::mono,
      {target}});
}

constexpr void GraphBuilderState::connect_sample_input(
    NodeBundlePortId target,
    SamplePortRef source)
{
  record_configured_sample_connection(target, source);
}

constexpr SamplePortRef GraphBuilderState::detach_sample_port(
    SamplePortRef const& source, size_t latency) {
  if (!source.graph_builder || source.graph_builder != &facade())
    details::error("cannot detach a sample port from another builder");
  auto const source_channels = source.channels();
  if (source_channels.empty())
    details::error("cannot detach a sample port with no semantic channels");
  if (_detach.reader_output_exists(source.channel_type, source_channels))
    return source;
  if (auto existing = _detach.info_for_source(source.channel_type, source_channels)) {
    if (existing->loop_extra_latency != latency)
      details::error("detach loop extra latency conflict");
    return SamplePortRef(
        facade(), {existing->reader_bundle, PortKind::sample, 0});
  }
  if (latency < 1)
    details::error("detach loop extra latency must be at least 1");
  auto id = _detach.allocate_detach_id();
  auto writer = NodeRef(
      facade(), _node_bundles.append_deferred_detach_writer(id, latency));
  record_configured_sample_connection(
      {writer.node_bundle_handle(), PortKind::sample, 0}, source);
  auto reader = NodeRef(
      facade(), _node_bundles.append_deferred_detach_reader(id, latency));
  SamplePortRef detached = static_cast<SamplePortRef>(reader);
  if (detached.channel_type != ChannelTypeId::mono ||
      detached.channels().size() != 1)
    details::error("detach reader must expose exactly one mono sample channel");
  _detach.record_detached_source({
      .detach_id = id,
      .source_type = source.channel_type,
      .source_channels = {source_channels.begin(), source_channels.end()},
      .writer_bundle = writer.node_bundle_handle(),
      .reader_bundle = reader.node_bundle_handle(),
      .reader_channel = detached.channels().front(),
      .loop_extra_latency = latency,
  });
  return detached;
}

constexpr SamplePortRef GraphBuilderState::lift_to_sample_port(
    NamedRef const& ref) {
  return std::visit(
      [&](auto const& value) -> SamplePortRef {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<T, EventPortRef>)
          details::error("expected sample value, got event");
        else if constexpr (std::same_as<T, Sample>)
          return static_cast<SamplePortRef>(
              facade().template node<Constant>(value));
        else
          return lift_to_sample_port(value);
      },
      ref.value);
}

} // namespace iv
