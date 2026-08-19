#include <intravenous/graph/builder.h>

#include <intravenous/basic_nodes/routing.h>
#include <intravenous/graph/builder/embedder.h>

namespace iv {
SamplePortRef::SamplePortRef(GraphBuilder &graph_builder_, size_t node_index,
                             size_t output_port)
    : graph_builder(&graph_builder_) {
  // Compatibility adapter only: translate a topology address immediately to
  // a logical identity when that mapping is unambiguous.
  if (node_index == GRAPH_ID) {
    *this = SamplePortRef(
        graph_builder_, GraphInputPortId{PortKind::sample, output_port});
    return;
  }
  auto const topology_port = TopologyPortId{node_index, output_port};
  if (graph_builder_._topology.is_scope_boundary_port(topology_port)) {
    *this = SamplePortRef(
        graph_builder_,
        ScopeBoundaryPortId::from_topology(topology_port, PortKind::sample));
    return;
  }

  if (node_index >= graph_builder_._topology.node_count()) {
    details::error("node at index " + std::to_string(node_index) +
                   " is out of bounds in builder " +
                   graph_builder_._identity.value);
  }
  auto const bundle =
      graph_builder_._node_bundles.bundle_for_concrete_node(node_index);
  NodeBundlePortId const logical{bundle, PortKind::sample, output_port};
  auto const descriptor = graph_builder_._node_bundles.resolve_sample_output(logical);
  if (descriptor.endpoints.size() != 1 ||
      descriptor.endpoints.front() != TopologyPortId{node_index, output_port}) {
    details::error(
        "raw topology sample output does not uniquely identify a logical "
        "NodeBundle output; use NodeBundlePortId");
  }
  *this = SamplePortRef(graph_builder_, logical);
}

SamplePortRef::SamplePortRef(GraphBuilder &graph_builder_,
                             GraphInputPortId graph_input)
    : graph_builder(&graph_builder_), graph_input_port(graph_input) {
  if (graph_input.port_kind != PortKind::sample) {
    details::error("attempted to create a sample ref from a graph event input");
  }
  if (graph_input.port_ordinal >= graph_builder_._public_ports.sample_inputs(graph_builder_._node_bundles).size()) {
    details::error(
        "graph input port " + std::to_string(graph_input.port_ordinal) +
        " is out of bounds in builder " + graph_builder_._identity.value);
  }
  auto const logical = NodeBundlePortId{
      graph_builder_._public_ports.boundary_handle(),
      PortKind::sample,
      graph_input.port_ordinal};
  auto semantic = SamplePortRef(graph_builder_, logical);
  node_bundle_port = logical;
  channel_type = semantic.channel_type;
  channels = std::move(semantic.channels);
}

SamplePortRef::SamplePortRef(GraphBuilder &graph_builder_,
                             ScopeBoundaryPortId scope_boundary)
    : graph_builder(&graph_builder_), scope_boundary_port(scope_boundary) {
  if (scope_boundary.port_kind != PortKind::sample) {
    details::error("attempted to create a sample ref from an event scope boundary");
  }
  if (!graph_builder_._topology.is_scope_boundary_port(
          scope_boundary.topology_port())) {
    details::error("attempted to create a sample ref from an unknown scope boundary");
  }

  std::optional<NodeBundlePortId> logical;
  for (NodeBundleHandle handle = 0; handle < graph_builder_._node_bundles.size(); ++handle) {
    auto const &bundle = graph_builder_._node_bundles.bundle(handle);
    if (!bundle.is_boundary()) continue;
    for (size_t port = 0; port < bundle.sample_output_count(); ++port) {
      auto const candidate = NodeBundlePortId{handle, PortKind::sample, port};
      auto const descriptor =
          graph_builder_._node_bundles.resolve_sample_output(candidate);
      if (std::find(descriptor.endpoints.begin(), descriptor.endpoints.end(),
                    scope_boundary.topology_port()) == descriptor.endpoints.end()) {
        continue;
      }
      if (logical) {
        details::error("scope boundary sample projection is ambiguous");
      }
      logical = candidate;
    }
  }
  if (logical) {
    auto semantic = SamplePortRef(graph_builder_, *logical);
    node_bundle_port = *logical;
    channel_type = semantic.channel_type;
    channels = std::move(semantic.channels);
  }
}

EventPortRef::EventPortRef(GraphBuilder &graph_builder_, size_t node_index,
                           size_t output_port)
    : graph_builder(&graph_builder_), node_index(node_index),
      output_port(output_port) {
  // Compatibility adapter for legacy boundary addresses. Normal graph/scope
  // input construction uses the explicit boundary-id constructors below.
  if (node_index == GRAPH_ID) {
    *this = EventPortRef(
        graph_builder_, GraphInputPortId{PortKind::event, output_port});
    return;
  }
  auto const topology_port = TopologyPortId{node_index, output_port};
  if (graph_builder_._topology.is_scope_boundary_port(topology_port)) {
    *this = EventPortRef(
        graph_builder_,
        ScopeBoundaryPortId::from_topology(topology_port, PortKind::event));
    return;
  }

  if (node_index >= graph_builder->_topology.node_count()) {
    details::error("node at index " + std::to_string(node_index) +
                   " "
                   "is out of bounds in builder " +
                   graph_builder->_identity.value);
  }

  auto const &ports = graph_builder->_topology.ports(node_index);
  size_t num_outputs = ports.event_outputs().size();
  if (output_port >= num_outputs) {
    details::error("event output port " + std::to_string(output_port) +
                   " of "
                   "node at index " +
                   std::to_string(node_index) +
                   " in "
                   "builder " +
                   graph_builder->_identity.value +
                   " "
                   "is out of bounds");
  }
  type = ports.event_outputs()[output_port].type;
  sources = graph_builder_._node_bundles.event_output_ports_for_topology_port(
      TopologyPortId{node_index, output_port});
}

EventPortRef::EventPortRef(GraphBuilder &graph_builder_,
                           GraphInputPortId graph_input)
    : graph_builder(&graph_builder_),
      node_index(graph_input.topology_port().node),
      output_port(graph_input.topology_port().port),
      graph_input_port(graph_input) {
  if (graph_input.port_kind != PortKind::event) {
    details::error("attempted to create an event ref from a graph sample input");
  }
  if (graph_input.port_ordinal >= graph_builder_._public_ports.event_inputs(graph_builder_._node_bundles).size()) {
    details::error("graph event input port " +
                   std::to_string(graph_input.port_ordinal) +
                   " is out of bounds in builder " +
                   graph_builder_._identity.value);
  }
  NodeBundlePortId const logical{
      graph_builder_._public_ports.boundary_handle(),
      PortKind::event,
      graph_input.port_ordinal,
  };
  auto const descriptor =
      graph_builder_._node_bundles.resolve_event_output(logical);
  type = descriptor.config.type;
  sources = graph_builder_._node_bundles.event_output_ports(logical);
}

EventPortRef::EventPortRef(GraphBuilder &graph_builder_,
                           ScopeBoundaryPortId scope_boundary)
    : graph_builder(&graph_builder_),
      node_index(scope_boundary.topology_port().node),
      output_port(scope_boundary.topology_port().port),
      scope_boundary_port(scope_boundary) {
  if (scope_boundary.port_kind != PortKind::event) {
    details::error("attempted to create an event ref from a sample scope boundary");
  }
  auto const topology_config =
      graph_builder_._topology.scope_boundary_event_output(scope_boundary);

  std::optional<NodeBundlePortId> logical;
  for (NodeBundleHandle handle = 0;
       handle < graph_builder_._node_bundles.size(); ++handle) {
    auto const &bundle = graph_builder_._node_bundles.bundle(handle);
    if (!bundle.is_boundary()) continue;
    for (size_t port = 0; port < bundle.event_output_count(); ++port) {
      auto const candidate =
          NodeBundlePortId{handle, PortKind::event, port};
      auto const descriptor =
          graph_builder_._node_bundles.resolve_event_output(candidate);
      if (std::find(descriptor.endpoints.begin(), descriptor.endpoints.end(),
                    scope_boundary.topology_port()) ==
          descriptor.endpoints.end()) {
        continue;
      }
      if (logical) {
        details::error("scope boundary event projection is ambiguous");
      }
      logical = candidate;
    }
  }
  if (logical) {
    auto const descriptor =
        graph_builder_._node_bundles.resolve_event_output(*logical);
    type = descriptor.config.type;
    sources = graph_builder_._node_bundles.event_output_ports(*logical);
  } else {
    // Compatibility fallback for a legacy topology-only scope boundary.
    type = topology_config.type;
  }
}

EventPortRef::EventPortRef(
    GraphBuilder &graph_builder_, EventTypeId type_,
    std::vector<EventOutputPortId> sources_,
    TopologyPortId topology_projection)
    : graph_builder(&graph_builder_),
      node_index(topology_projection.node),
      output_port(topology_projection.port),
      type(type_),
      sources(std::move(sources_)) {
  if (sources.empty()) {
    details::error("event expression has no semantic sources");
  }
  for (auto const source : sources) {
    auto const descriptor = graph_builder_._node_bundles.resolve_event_output(
        NodeBundlePortId{source.bundle, PortKind::event, source.port});
    if (descriptor.config.type != type) {
      details::error(
          "event expression source does not match its semantic event type");
    }
  }
}

SamplePortRef::SamplePortRef(GraphBuilder &graph_builder_,
                             NodeBundlePortId bundle_port)
    : graph_builder(&graph_builder_), node_bundle_port(bundle_port) {
  if (bundle_port.port_kind != PortKind::sample) {
    details::error("attempted to create a SamplePortRef from an event NodeBundle port");
  }
  auto const descriptor =
      graph_builder_._node_bundles.resolve_sample_output(bundle_port);
  channel_type = descriptor.config.channel_layout.channel_type;
  channels = graph_builder_._node_bundles.sample_output_channels(bundle_port);
}

SamplePortRef::SamplePortRef(
    GraphBuilder &graph_builder_, ChannelTypeId channel_type_,
    std::vector<SampleOutputChannelId> channels_)
    : graph_builder(&graph_builder_), channel_type(channel_type_),
      channels(std::move(channels_)) {
  auto const expected = channel_count(channel_type);
  if (channels.size() != expected) {
    details::error(
        "sample expression channel count does not match its semantic channel type");
  }
  for (auto const channel : channels) {
    auto const descriptor = graph_builder_._node_bundles.resolve_sample_output(
        NodeBundlePortId{channel.bundle, PortKind::sample, channel.port});
    if (channel.channel >=
        channel_count(descriptor.config.channel_layout.channel_type)) {
      details::error("sample expression channel is out of bounds");
    }
  }
}

SamplePortRef::operator TopologyPortId() const {
  if (graph_input_port) return graph_input_port->topology_port();
  if (scope_boundary_port) return scope_boundary_port->topology_port();
  details::error(
      "a logical node output SamplePortRef must be materialized by GraphBuilder");
}

SamplePortRef SamplePortRef::_clone_handle() const {
  return *this;
}

SamplePortRef SamplePortRef::select_channel(size_t channel) const {
  if (!graph_builder) {
    details::error("attempted to select a channel from an empty sample port");
  }
  if (channel >= channels.size()) {
    details::error("sample channel ordinal is out of bounds");
  }
  return SamplePortRef(
      *graph_builder, ChannelTypeId::mono,
      std::vector<SampleOutputChannelId>{channels[channel]});
}

SamplePortRef SamplePortRef::detach(size_t loop_extra_latency) const {
  if (!graph_builder) {
    details::error("attempted to detach an empty sample port");
  }
  return graph_builder->detach_sample_port(*this, loop_extra_latency);
}

std::string SamplePortRef::to_string() const {
  if (!graph_builder) {
    return "empty sample port";
  }
  if (graph_input_port) {
    return "graph input " + std::to_string(graph_input_port->port_ordinal) +
           " in builder " + graph_builder->_identity.value;
  }
  if (scope_boundary_port) {
    return "subgraph sample input " +
           std::to_string(scope_boundary_port->boundary_ordinal) +
           " in builder " + graph_builder->_identity.value;
  }
  if (node_bundle_port) {
    return "sample output " + std::to_string(node_bundle_port->port_ordinal) +
           " of node bundle " + std::to_string(node_bundle_port->node_bundle_handle);
  }
  if (!channels.empty()) {
    return "structural sample expression with " +
           std::to_string(channels.size()) + " channel(s) in builder " +
           graph_builder->_identity.value;
  }
  return "invalid sample port in builder " + graph_builder->_identity.value;
}

std::string EventPortRef::to_string() const {
  if (!graph_builder) {
    return "empty event";
  }
  if (graph_input_port) {
    return "graph event input " +
           std::to_string(graph_input_port->port_ordinal) + " in builder " +
           graph_builder->_identity.value;
  }
  if (scope_boundary_port) {
    return "subgraph event input " +
           std::to_string(scope_boundary_port->boundary_ordinal) +
           " in builder " + graph_builder->_identity.value;
  }
  if (sources.size() > 1) {
    return "structural event expression with " +
           std::to_string(sources.size()) + " source(s) in builder " +
           graph_builder->_identity.value;
  }
  return "event at address " + graph_builder->node_id(node_index) + ":" +
         std::to_string(output_port);
}

GraphBuilderIdentity::GraphBuilderIdentity(std::string value_)
    : value(std::move(value_)) {}

std::string GraphBuilderIdentity::child_id(size_t index) const {
  std::string nested_path = value;
  if (!nested_path.empty()) {
    nested_path += ".";
  }
  nested_path += std::to_string(index);
  return nested_path;
}

GraphBuilder::GraphBuilder(GraphBuilderIdentity identity)
    : _identity(std::move(identity)),
      _public_ports(_node_bundles.append_boundary()) {}

GraphBuilder::GraphBuilder()
    : GraphBuilder(GraphBuilderIdentity(allocate_root_builder_id())) {}

GraphBuilder GraphBuilder::derive_nested_builder() {
  return GraphBuilder(
      GraphBuilderIdentity(_identity.child_id(_topology.node_count())));
}

bool GraphBuilder::inside_subgraph_scope() const { return _subgraphs.active(); }

ScopedSubgraph &GraphBuilder::current_scope() { return _subgraphs.current(); }

void GraphBuilder::define_scope_outputs(std::span<OutputRefConfig const> refs) {
  _subgraphs.define_sample_outputs(
      refs, *this, _topology, _node_bundles, _identity);
}

void GraphBuilder::define_scope_event_outputs(
    std::span<EventOutputRefConfig const> refs) {
  _subgraphs.define_event_outputs(
      refs, *this, _topology, _node_bundles, _identity,
      _public_ports.event_inputs(_node_bundles));
}

std::string GraphBuilder::node_id(size_t index) const {
  IV_ASSERT(index < _topology.node_count(), "node index out of bounds");
  return _identity.child_id(index);
}

PublicSampleInputRef GraphBuilder::input() { return input(Sample{0.0f}); }

PublicSampleInputRef GraphBuilder::input_named(std::string_view name,
                                               Sample default_value,
                                               std::optional<Sample> min,
                                               std::optional<Sample> max) {
  if (inside_subgraph_scope()) {
    return PublicSampleInputRef(_subgraphs.add_scope_sample_input(
        *this, _topology, _node_bundles, name, default_value, min, max, true));
  }
  return PublicSampleInputRef(
      _public_ports.add_sample_input(*this, _node_bundles, name, default_value, min, max));
}

PublicSampleInputRef GraphBuilder::input(Sample default_value,
                                         std::optional<Sample> min,
                                         std::optional<Sample> max) {
  if (inside_subgraph_scope()) {
    return PublicSampleInputRef(_subgraphs.add_scope_sample_input(
        *this, _topology, _node_bundles, {}, default_value, min, max, false));
  }
  return PublicSampleInputRef(
      _public_ports.add_sample_input(*this, _node_bundles, {}, default_value, min, max));
}

void GraphBuilder::annotate_public_sample_input_source_info(
    PublicSampleInputRef const &ref, std::string_view declaration_identity,
    std::string_view file_path, uint32_t begin, uint32_t end) {
  if (declaration_identity.empty() || ref.port.graph_builder != this) {
    return;
  }
  if (ref.port.graph_input_port) {
    _public_ports.annotate_sample_input_source_info(
        ref.port.graph_input_port->port_ordinal, declaration_identity,
        file_path, begin, end);
    return;
  }
  if (ref.port.node_bundle_port) {
    _subgraphs.annotate_scope_input_source_info(
        *ref.port.node_bundle_port, _node_bundles,
        SourceInfo{
            .declaration_identity = std::string(declaration_identity),
            .span = SourceSpan{
                .file_path = std::string(file_path),
                .begin = begin,
                .end = end,
            },
        });
    return;
  }
  if (ref.port.scope_boundary_port) {
    _subgraphs.annotate_scope_input_source_info(
        *ref.port.scope_boundary_port, _node_bundles,
        SourceInfo{
            .declaration_identity = std::string(declaration_identity),
            .span = SourceSpan{
                .file_path = std::string(file_path),
                .begin = begin,
                .end = end,
            },
        });
    return;
  }
  details::error("PublicSampleInputRef does not refer to a graph or scope input");
}

void PublicSampleInputRef::_annotate_source_info(
    std::string_view declaration_identity, std::string_view file_path,
    uint32_t begin, uint32_t end) const {
  if (port.graph_builder != nullptr) {
    port.graph_builder->annotate_public_sample_input_source_info(
        *this, declaration_identity, file_path, begin, end);
  }
}

PublicEventInputRef GraphBuilder::event_input_named(std::string_view name,
                                                    EventTypeId type) {
  if (inside_subgraph_scope()) {
    return PublicEventInputRef(
        _subgraphs.add_scope_event_input(*this, _topology, _node_bundles, name, type, true));
  }
  return PublicEventInputRef(_public_ports.add_event_input(*this, _node_bundles, name, type));
}

PublicEventInputRef GraphBuilder::event_input(EventTypeId type) {
  if (inside_subgraph_scope()) {
    return PublicEventInputRef(
        _subgraphs.add_scope_event_input(*this, _topology, _node_bundles, {}, type, false));
  }
  return PublicEventInputRef(_public_ports.add_event_input(*this, _node_bundles, {}, type));
}

void GraphBuilder::annotate_public_event_input_source_info(
    PublicEventInputRef const &ref, std::string_view identity,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (identity.empty() || ref.port.graph_builder != this)
    return;
  if (ref.port.graph_input_port) {
    _public_ports.annotate_event_input_source_info(
        ref.port.graph_input_port->port_ordinal, identity, file, begin, end);
    return;
  }
  if (ref.port.scope_boundary_port) {
    _subgraphs.annotate_scope_input_source_info(
        *ref.port.scope_boundary_port, _node_bundles,
        SourceInfo{
            .declaration_identity = std::string(identity),
            .span = SourceSpan{
                .file_path = std::string(file),
                .begin = begin,
                .end = end,
            },
        });
    return;
  }
  details::error("PublicEventInputRef does not refer to a graph or scope input");
}

void PublicEventInputRef::_annotate_source_info(std::string_view identity,
                                                std::string_view file,
                                                uint32_t begin,
                                                uint32_t end) const {
  if (port.graph_builder)
    port.graph_builder->annotate_public_event_input_source_info(
        *this, identity, file, begin, end);
}

void GraphBuilder::annotate_public_sample_output_source_info(
    std::span<SourceInfo const> infos) {
  for (size_t i = 0; i < infos.size(); ++i)
    _public_ports.annotate_sample_output_source_info(i, infos[i]);
}

void GraphBuilder::annotate_public_event_output_source_info(
    std::span<SourceInfo const> infos) {
  for (size_t i = 0; i < infos.size(); ++i)
    _public_ports.annotate_event_output_source_info(i, infos[i]);
}

NodeRef GraphBuilder::embed_subgraph(GraphBuilder const &child) {
  if (!child._public_ports.sample_outputs_defined()) {
    details::error("builder " + child._identity.value +
                   ": g.outputs(...) must be called before insertion");
  }

  size_t const child_node_bundle_offset = _node_bundles.size();
  size_t const subgraph_node = GraphBuilderChildEmbedder::embed(
      _topology, _node_bundles, _connections, _detach, _virtual_nodes,
      child._public_ports, child._topology, child._node_bundles,
      child._connections, child._detach, child._virtual_nodes);
  auto const child_boundary =
      child_node_bundle_offset + child._public_ports.boundary_handle();
  return NodeRef(
      *this,
      _node_bundles.append_subgraph(_topology, subgraph_node, child_boundary));
}

void GraphBuilder::event_outputs(std::span<EventOutputRefConfig const> refs) {
  _public_ports.define_event_outputs(*this, _topology, _node_bundles, _identity, refs);
}

void GraphBuilder::outputs(std::initializer_list<NamedRef> refs) {
  _public_ports.define_sample_outputs_from_named_refs(
      *this, _topology, _node_bundles, _identity,
      [&](auto &&value) {
        return lift_to_sample_port(std::forward<decltype(value)>(value));
      },
      std::span<NamedRef const>(refs.begin(), refs.size()));
}

void GraphBuilder::outputs(std::span<OutputRefConfig const> refs) {
  _public_ports.define_sample_outputs(*this, _topology, _node_bundles, _identity, refs);
}

void GraphBuilder::outputs(std::span<NamedRef const> refs) {
  _public_ports.define_sample_outputs_from_named_refs(
      *this, _topology, _node_bundles, _identity,
      [&](auto &&value) {
        return lift_to_sample_port(std::forward<decltype(value)>(value));
      },
      refs);
}

void GraphBuilder::subgraph_event_outputs(
    std::span<EventOutputRefConfig const> refs) {
  if (!inside_subgraph_scope()) {
    details::error(
        "g.subgraph_event_outputs(...) is only valid inside g.subgraph(...)");
  }
  define_scope_event_outputs(refs);
}

void GraphBuilder::subgraph_outputs(std::initializer_list<NamedRef> refs) {
  subgraph_outputs(std::span<NamedRef const>(refs.begin(), refs.size()));
}

void GraphBuilder::subgraph_outputs(std::span<OutputRefConfig const> refs) {
  if (!inside_subgraph_scope()) {
    details::error(
        "g.subgraph_outputs(...) is only valid inside g.subgraph(...)");
  }
  define_scope_outputs(refs);
}

void GraphBuilder::subgraph_outputs(std::span<NamedRef const> refs) {
  if (!inside_subgraph_scope()) {
    details::error(
        "g.subgraph_outputs(...) is only valid inside g.subgraph(...)");
  }
  _subgraphs.define_sample_outputs_from_named_refs(
      *this, _topology, _node_bundles, _identity,
      [&](auto &&value) {
        return lift_to_sample_port(std::forward<decltype(value)>(value));
      },
      refs);
}

std::string GraphBuilder::allocate_root_builder_id() {
  static size_t next_root_builder_id = 0;
  return std::to_string(next_root_builder_id++);
}

SamplePortRef GraphBuilder::detach_sample_port(SamplePortRef const &sample_port,
                                               size_t loop_extra_latency) {
  if (!sample_port.graph_builder) {
    details::error("builder " + _identity.value +
                   ": cannot detach an empty sample port");
  }
  if (sample_port.graph_builder != this) {
    details::error("builder " + _identity.value + ": cannot detach " +
                   sample_port.to_string() +
                   " because it belongs to another builder");
  }
  if (sample_port.channels.empty()) {
    details::error("builder " + _identity.value +
                   ": cannot detach a sample port with no semantic channels");
  }

  if (_detach.reader_output_exists(
          sample_port.channel_type, sample_port.channels)) {
    return sample_port;
  }
  if (auto const *existing = _detach.info_for_source(
          sample_port.channel_type, sample_port.channels)) {
    if (existing->loop_extra_latency != loop_extra_latency) {
      details::error("builder " + _identity.value +
                     ": detach loop extra latency conflict on " +
                     sample_port.to_string());
    }
    return SamplePortRef(
        *this,
        NodeBundlePortId{
            existing->reader_bundle, PortKind::sample, 0});
  }
  if (loop_extra_latency < 1) {
    details::error("builder " + _identity.value +
                   ": detach loop extra latency must be at least 1");
  }

  size_t const detach_id = _detach.allocate_detach_id();
  auto writer = node<DetachWriterNode>(detach_id, loop_extra_latency);
  record_authored_sample_connection(
      NodeBundlePortId{
          writer.node_bundle_handle(), PortKind::sample, 0},
      sample_port);

  auto reader = node<DetachReaderNode>(detach_id, loop_extra_latency);
  SamplePortRef detached = static_cast<SamplePortRef>(reader);
  if (detached.channel_type != ChannelTypeId::mono ||
      detached.channels.size() != 1) {
    details::error("detach reader must expose exactly one mono sample channel");
  }

  _detach.record_detached_source(AuthoredDetachedSamplePortInfo{
      .detach_id = detach_id,
      .source_type = sample_port.channel_type,
      .source_channels = sample_port.channels,
      .writer_bundle = writer.node_bundle_handle(),
      .reader_bundle = reader.node_bundle_handle(),
      .reader_channel = detached.channels.front(),
      .loop_extra_latency = loop_extra_latency,
  });
  return detached;
}

GraphBuilder::VacantInputs GraphBuilder::vacant_inputs() const {
  return _connections.collect_vacant_inputs(
      _topology, _node_bundles, _virtual_nodes);
}

GraphBuilder::VirtualInputs GraphBuilder::virtual_inputs() const {
  return _connections.collect_virtual_inputs(
      _topology, _node_bundles, _virtual_nodes);
}

GraphBuilder::VirtualSampleInputFamilies
GraphBuilder::virtual_sample_input_families() const {
  return _connections.collect_virtual_sample_input_families(_node_bundles,
                                                            _virtual_nodes);
}

GraphBuilder::VirtualOutputs GraphBuilder::virtual_outputs() const {
  return _connections.collect_virtual_outputs(
      _topology, _node_bundles, _virtual_nodes);
}

GraphBuilder::VirtualSampleOutputFamilies
GraphBuilder::virtual_sample_output_families() const {
  return _connections.collect_virtual_sample_output_families(
      _node_bundles, _virtual_nodes);
}

GraphBuilder::VirtualPorts GraphBuilder::virtual_ports() const {
  return _virtual_nodes.ports(_topology, _node_bundles);
}

GraphBuilderPublicSamplePortFamilies
GraphBuilder::public_sample_input_families() const {
  return _public_ports.sample_input_families(_node_bundles);
}

bool GraphBuilder::public_sample_input_is_connected(size_t port_ordinal) const {
  auto const source = NodeBundlePortId{
      _public_ports.boundary_handle(), PortKind::sample, port_ordinal};
  auto const channels = _node_bundles.sample_output_channels(source);
  return std::ranges::any_of(channels, [&](auto channel) {
    return _connections.sample_output_is_connected(channel);
  });
}

std::vector<GraphBuilderPublicEventInput>
GraphBuilder::public_event_inputs() const {
  return _public_ports.collected_event_inputs(_node_bundles);
}

bool GraphBuilder::public_event_input_is_connected(size_t port_ordinal) const {
  auto const source =
      GraphInputPortId{PortKind::event, port_ordinal}.topology_port();
  bool connected = false;
  _topology.for_each_event_edge([&](TopologyEventEdge const &edge) {
    connected = connected || (edge.source == source);
  });
  return connected;
}

std::span<SourceInfo const>
GraphBuilder::public_event_input_source_infos(size_t port_ordinal) const {
  return _public_ports.event_input_source_infos(port_ordinal);
}

GraphBuilderPublicSamplePortFamilies
GraphBuilder::public_sample_output_families() const {
  return _public_ports.sample_output_families(_node_bundles);
}

std::vector<GraphBuilderPublicEventOutput>
GraphBuilder::public_event_outputs() const {
  return _public_ports.collected_event_outputs(_node_bundles);
}

void GraphBuilder::record_authored_sample_connection(
    NodeBundlePortId target, SamplePortRef const &source) {
  if (target.port_kind != PortKind::sample) {
    details::error(
        "attempted to record a sample connection to an event NodeBundle port");
  }
  if (!source.graph_builder) {
    details::error("cannot connect an empty sample output");
  }
  if (source.graph_builder != this) {
    details::error("cannot connect a sample output from another builder");
  }
  auto source_type = source.channel_type;
  auto source_channels = source.channels;
  if (source_channels.size() != channel_count(source_type) &&
      source.node_bundle_port) {
    auto const descriptor =
        _node_bundles.resolve_sample_output(*source.node_bundle_port);
    source_type = descriptor.config.channel_layout.channel_type;
    source_channels =
        _node_bundles.sample_output_channels(*source.node_bundle_port);
  }

  if (source_channels.size() != channel_count(source_type)) {
    details::error(
        "sample source does not match its semantic channel type");
  }

  auto const target_descriptor = _node_bundles.resolve_sample_input(target);
  _connections.record_authored_sample_connection(AuthoredSampleConnection{
      .source_type = source_type,
      .source_channels = std::move(source_channels),
      .target_type = target_descriptor.config.channel_layout.channel_type,
      .target_channels = _node_bundles.sample_input_channels(target),
  });
}

void GraphBuilder::record_authored_sample_connection(
    SampleInputChannelId target, SamplePortRef const &source) {
  if (!source.graph_builder) {
    details::error("cannot connect an empty sample output");
  }
  if (source.graph_builder != this) {
    details::error("cannot connect a sample output from another builder");
  }

  NodeBundlePortId const target_port{
      target.bundle, PortKind::sample, target.port};
  auto const target_channels = _node_bundles.sample_input_channels(target_port);
  if (target.channel >= target_channels.size() ||
      target_channels[target.channel] != target) {
    details::error("sample input channel does not belong to its NodeBundle port");
  }

  auto source_type = source.channel_type;
  auto source_channels = source.channels;
  if (source_channels.size() != channel_count(source_type) &&
      source.node_bundle_port) {
    auto const descriptor =
        _node_bundles.resolve_sample_output(*source.node_bundle_port);
    source_type = descriptor.config.channel_layout.channel_type;
    source_channels =
        _node_bundles.sample_output_channels(*source.node_bundle_port);
  }
  if (source_channels.size() != channel_count(source_type)) {
    details::error(
        "sample source does not match its semantic channel type");
  }

  _connections.record_authored_sample_connection(AuthoredSampleConnection{
      .source_type = source_type,
      .source_channels = std::move(source_channels),
      .target_type = ChannelTypeId::mono,
      .target_channels = {target},
  });
}

void GraphBuilder::record_authored_sample_connection(
    NodeBundlePortId target, std::span<SamplePortRef const> sources) {
  if (target.port_kind != PortKind::sample) {
    details::error(
        "attempted to record sample channels to an event NodeBundle port");
  }

  auto const target_descriptor = _node_bundles.resolve_sample_input(target);
  auto target_channels = _node_bundles.sample_input_channels(target);
  if (sources.size() != target_channels.size()) {
    details::error(
        "sample channel source count does not match NodeBundle port layout");
  }

  std::vector<SampleOutputChannelId> source_channels;
  source_channels.reserve(sources.size());
  for (auto const &source : sources) {
    if (!source.graph_builder) {
      details::error("cannot connect an empty sample output");
    }
    if (source.graph_builder != this) {
      details::error("cannot connect a sample output from another builder");
    }
    if (source.channel_type != ChannelTypeId::mono ||
        source.channels.size() != 1) {
      details::error(
          "channel-wise sample connection requires one scalar source per channel");
    }
    source_channels.push_back(source.channels.front());
  }

  _connections.record_authored_sample_connection(AuthoredSampleConnection{
      .source_type = target_descriptor.config.channel_layout.channel_type,
      .source_channels = std::move(source_channels),
      .target_type = target_descriptor.config.channel_layout.channel_type,
      .target_channels = std::move(target_channels),
  });
}

void GraphBuilder::record_authored_event_connection(
    NodeBundlePortId target, EventPortRef const &source) {
  if (target.port_kind != PortKind::event) {
    details::error(
        "attempted to record an event connection to a sample NodeBundle port");
  }
  if (!source.graph_builder) {
    details::error("cannot connect an empty event output");
  }
  if (source.graph_builder != this) {
    details::error("cannot connect an event output from another builder");
  }
  if (source.sources.empty()) {
    details::error("event source has no semantic port identity");
  }

  for (auto const source_port : source.sources) {
    auto const descriptor = _node_bundles.resolve_event_output(
        NodeBundlePortId{
            source_port.bundle, PortKind::event, source_port.port});
    if (descriptor.config.type != source.type) {
      details::error(
          "event source does not match its semantic event type");
    }
  }

  auto const target_descriptor = _node_bundles.resolve_event_input(target);
  auto target_ports = _node_bundles.event_input_ports(target);
  for (auto const target_port : target_ports) {
    auto const descriptor = _node_bundles.resolve_event_input(
        NodeBundlePortId{
            target_port.bundle, PortKind::event, target_port.port});
    if (descriptor.config.type != target_descriptor.config.type) {
      details::error(
          "event target does not match its semantic event type");
    }
  }

  _connections.record_authored_event_connection(AuthoredEventConnection{
      .source_type = source.type,
      .sources = source.sources,
      .target_type = target_descriptor.config.type,
      .targets = std::move(target_ports),
  });
}

GraphBuilder GraphBuilder::completed_sample_builder() const {
  GraphBuilder completed = *this;
  completed.materialize_authored_sample_connections_for_completion();
  return completed;
}

void GraphBuilder::materialize_authored_sample_connections_for_completion() {
  struct SubgraphBoundary {
    NodeBundleHandle boundary = 0;
    size_t node = 0;
    std::vector<bool> output_assigned{};
  };

  std::vector<SubgraphBoundary> subgraphs;
  std::unordered_map<NodeBundleHandle, size_t> subgraph_index_by_boundary;
  for (NodeBundleHandle handle = 0; handle < _node_bundles.size(); ++handle) {
    auto const boundary =
        _node_bundles.bundle(handle).subgraph_boundary_handle();
    if (!boundary) continue;

    std::optional<size_t> topology_node;
    _node_bundles.bundle(handle).for_each_topology_node([&](size_t node) {
      if (topology_node) {
        details::error(
            "SubgraphNodeBundle has more than one topology node");
      }
      topology_node = node;
    });
    if (!topology_node) {
      details::error("SubgraphNodeBundle has no topology node");
    }
    if (subgraph_index_by_boundary.contains(*boundary)) {
      details::error(
          "BoundaryNodeBundle is owned by more than one SubgraphNodeBundle");
    }

    auto const output_count =
        _node_bundles.bundle(*boundary).boundary_sample_outputs().size();
    subgraph_index_by_boundary.emplace(*boundary, subgraphs.size());
    subgraphs.push_back(SubgraphBoundary{
        .boundary = *boundary,
        .node = *topology_node,
        .output_assigned = std::vector<bool>(output_count, false),
    });
  }

  // Imported boundaries deliberately lose their child topology projection.
  // Give every completed subgraph boundary a fresh lowering-only source so
  // the existing pack/unpack materializer can be reused unchanged.
  for (auto const &subgraph : subgraphs) {
    auto &binding = _topology.subgraph_node(subgraph.node).lowered_subgraph;
    auto const input_count =
        _node_bundles.bundle(subgraph.boundary).boundary_sample_inputs().size();
    auto const output_count =
        _node_bundles.bundle(subgraph.boundary).boundary_sample_outputs().size();
    if (binding.sample_input_targets.size() != input_count) {
      details::error(
          "SubgraphNode sample input binding does not match its boundary");
    }
    binding.sample_output_sources =
        std::vector<TopologyPortId>(output_count);

    for (size_t input = 0; input < input_count; ++input) {
      NodeBundlePortId const logical{
          subgraph.boundary, PortKind::sample, input};
      auto descriptor = _node_bundles.resolve_sample_output(logical);
      if (descriptor.endpoints.empty()) {
        auto const projection =
            _topology.append_scope_sample_input(descriptor.config);
        _node_bundles.bundle(subgraph.boundary)
            .set_boundary_sample_input_projection(
                input, projection.topology_port());
        descriptor = _node_bundles.resolve_sample_output(logical);
      }
      if (descriptor.endpoints.size() != 1) {
        details::error(
            "subgraph boundary sample input does not have exactly one "
            "completion topology projection");
      }
    }
  }

  auto target_port = [&](AuthoredSampleConnection const &connection) {
    if (connection.target_channels.empty()) {
      details::error("authored sample connection has no target channels");
    }
    auto const first = connection.target_channels.front();
    for (auto const channel : connection.target_channels) {
      if (channel.bundle != first.bundle || channel.port != first.port) {
        details::error(
            "authored sample connection target spans multiple bundle ports");
      }
    }

    NodeBundlePortId const target{
        first.bundle, PortKind::sample, first.port};
    auto const descriptor = _node_bundles.resolve_sample_input(target);
    auto const target_channels = _node_bundles.sample_input_channels(target);
    bool const whole_port =
        descriptor.config.channel_layout.channel_type == connection.target_type &&
        target_channels == connection.target_channels;
    bool const scalar_channel =
        connection.target_type == ChannelTypeId::mono &&
        connection.target_channels.size() == 1 &&
        std::ranges::contains(
            target_channels, connection.target_channels.front());
    if (!whole_port && !scalar_channel) {
      details::error(
          "authored sample connection target no longer matches its "
          "NodeBundle port");
    }
    return target;
  };

  auto targets_whole_port =
      [&](NodeBundlePortId target,
          AuthoredSampleConnection const &connection) {
    auto const descriptor = _node_bundles.resolve_sample_input(target);
    return connection.target_type ==
               descriptor.config.channel_layout.channel_type &&
           connection.target_channels ==
               _node_bundles.sample_input_channels(target);
  };

  struct SampleTargetGroup {
    NodeBundlePortId target{};
    std::vector<AuthoredSampleConnection const *> connections{};
  };
  std::vector<SampleTargetGroup> target_groups;
  for (auto const &connection : _connections.authored_sample_connections()) {
    auto const target = target_port(connection);
    if (targets_whole_port(target, connection)) {
      // Keep ordinary whole-port connections independent. Grouping exists only
      // to combine channel-qualified contributors to one wider destination.
      target_groups.push_back(SampleTargetGroup{
          .target = target,
          .connections = {&connection},
      });
      continue;
    }

    auto group = std::find_if(
        target_groups.begin(), target_groups.end(),
        [&](SampleTargetGroup const &candidate) {
          return candidate.target == target &&
                 !targets_whole_port(
                     candidate.target, *candidate.connections.front());
        });
    if (group == target_groups.end()) {
      target_groups.push_back(SampleTargetGroup{.target = target});
      group = target_groups.end() - 1;
    }
    group->connections.push_back(&connection);
  }

  std::vector<std::pair<NodeBundlePortId, std::vector<SamplePortRef>>>
      projected_source_channels;
  auto source_ref = [&](AuthoredSampleConnection const &connection) {
    if (connection.source_channels.empty()) {
      details::error("authored sample connection has no source channels");
    }
    auto const first = connection.source_channels.front();

    // Scalar selections from one native multichannel topology port share one
    // compatibility unpack for the whole completion pass. This preserves the
    // old graph-service lowering, which projected all channels together.
    if (connection.source_type == ChannelTypeId::mono &&
        connection.source_channels.size() == 1) {
      NodeBundlePortId const logical{
          first.bundle, PortKind::sample, first.port};
      auto const descriptor = _node_bundles.resolve_sample_output(logical);
      auto const physical_channel_count =
          channel_count(descriptor.config.channel_layout.channel_type);
      if (physical_channel_count > 1 && descriptor.endpoints.size() == 1) {
        auto projected = std::find_if(
            projected_source_channels.begin(),
            projected_source_channels.end(),
            [&](auto const &entry) { return entry.first == logical; });
        if (projected == projected_source_channels.end()) {
          auto materialized =
              materialize_bundle_sample_output_channels(logical);
          std::vector<SamplePortRef> refs;
          refs.reserve(materialized.size());
          for (auto const port : materialized) {
            refs.emplace_back(*this, port.port.node, port.port.port);
          }
          projected_source_channels.emplace_back(logical, std::move(refs));
          projected = projected_source_channels.end() - 1;
        }
        if (first.channel >= projected->second.size()) {
          details::error("authored sample source channel is out of bounds");
        }
        return projected->second[first.channel];
      }
    }
    bool const one_bundle_port = std::ranges::all_of(
        connection.source_channels, [&](SampleOutputChannelId channel) {
          return channel.bundle == first.bundle && channel.port == first.port;
        });
    if (one_bundle_port) {
      NodeBundlePortId const logical{
          first.bundle, PortKind::sample, first.port};
      auto const descriptor = _node_bundles.resolve_sample_output(logical);
      if (descriptor.config.channel_layout.channel_type ==
              connection.source_type &&
          _node_bundles.sample_output_channels(logical) ==
              connection.source_channels) {
        return SamplePortRef(*this, logical);
      }
    }
    return SamplePortRef(
        *this, connection.source_type, connection.source_channels);
  };

  auto source_for_target_group =
      [&](SampleTargetGroup const &group) -> SamplePortRef {
    auto const descriptor = _node_bundles.resolve_sample_input(group.target);
    auto const target_channels =
        _node_bundles.sample_input_channels(group.target);

    if (group.connections.size() == 1) {
      auto const &connection = *group.connections.front();
      if (targets_whole_port(group.target, connection)) {
        return source_ref(connection);
      }
    }

    std::vector<bool> assigned(target_channels.size(), false);
    auto connect_partial_channels = [&](NodeBundleHandle pack_bundle) {
      for (auto const *connection : group.connections) {
        if (connection->target_type != ChannelTypeId::mono ||
            connection->target_channels.size() != 1) {
          details::error(
              "sample bundle port mixes whole-port and channel contributors");
        }
        auto const selected = connection->target_channels.front();
        auto const target_it =
            std::ranges::find(target_channels, selected);
        if (target_it == target_channels.end()) {
          details::error(
              "sample channel contributor does not belong to its target port");
        }
        auto const channel = static_cast<size_t>(
            target_it - target_channels.begin());
        if (assigned[channel]) {
          details::error(
              "sample bundle port channel has more than one authored source");
        }
        assigned[channel] = true;
        connect_sample_input_lowered(
            NodeBundlePortId{
                pack_bundle, PortKind::sample, channel},
            source_ref(*connection));
      }
    };

    switch (descriptor.config.channel_layout.channel_type) {
    case ChannelTypeId::stereo: {
      auto pack = node<ChannelPack<stereo>>();
      connect_partial_channels(pack.node_bundle_handle());
      return static_cast<SamplePortRef>(pack);
    }
    case ChannelTypeId::mono:
      details::error(
          "mono sample input has more than one authored source");
    case ChannelTypeId::count:
      break;
    }
    details::error("invalid channel type for partial sample target");
  };

  // Re-run only the compatibility lowering side. The authored records are
  // already authoritative and must not be duplicated by completion.
  for (auto const &group : target_groups) {
    auto const target = group.target;
    if (subgraph_index_by_boundary.contains(target.node_bundle_handle)) {
      // A non-root boundary input is the inward side of a subgraph output.
      // It has no standalone topology endpoint; its source is installed in
      // the SubgraphNode binding below.
      continue;
    }

    auto const source = source_for_target_group(group);
    auto const target_descriptor = _node_bundles.resolve_sample_input(target);
    if (target_descriptor.endpoints.size() == 1 &&
        target_descriptor.endpoints.front().node == GRAPH_ID) {
      _topology.add_sample_edge(TopologyEdge{
          materialize_sample_output(source).port,
          target_descriptor.endpoints.front(),
      });
      continue;
    }
    connect_sample_input_lowered(target, source);
  }

  // Connections targeting a non-root BoundaryNodeBundle define the source of
  // the corresponding parent-facing SubgraphNode output.
  for (auto const &group : target_groups) {
    auto const target = group.target;
    auto const subgraph_it =
        subgraph_index_by_boundary.find(target.node_bundle_handle);
    if (subgraph_it == subgraph_index_by_boundary.end()) continue;

    auto &subgraph = subgraphs[subgraph_it->second];
    if (target.port_ordinal >= subgraph.output_assigned.size()) {
      details::error("subgraph sample output ordinal is out of bounds");
    }
    if (subgraph.output_assigned[target.port_ordinal]) {
      details::error("subgraph sample output has more than one source");
    }
    auto const materialized =
        materialize_sample_output(source_for_target_group(group)).port;
    _topology.subgraph_node(subgraph.node)
        .lowered_subgraph.sample_output_sources[target.port_ordinal] =
        materialized;
    subgraph.output_assigned[target.port_ordinal] = true;
  }

  // Connections sourced by a subgraph boundary were replayed through its
  // temporary completion projection. Project those edges into the SubgraphNode
  // input binding. Keep the completion edges as provenance for nested
  // passthrough outputs; PreparedBuilderGraph skips them as executable edges.
  for (auto const &subgraph : subgraphs) {
    auto &binding = _topology.subgraph_node(subgraph.node).lowered_subgraph;
    for (size_t input = 0; input < binding.sample_input_targets.size();
         ++input) {
      auto const descriptor = _node_bundles.resolve_sample_output(
          NodeBundlePortId{
              subgraph.boundary, PortKind::sample, input});
      if (descriptor.endpoints.size() != 1) {
        details::error(
            "subgraph boundary sample input lost its completion projection");
      }
      auto const projection = descriptor.endpoints.front();
      _topology.for_each_sample_edge([&](TopologyEdge const &edge) {
        if (edge.source != projection) return;
        auto &targets = binding.sample_input_targets[input];
        if (std::find(targets.begin(), targets.end(), edge.target) ==
            targets.end()) {
          targets.push_back(edge.target);
        }
      });
    }
  }

  for (auto const &subgraph : subgraphs) {
    if (std::ranges::any_of(
            subgraph.output_assigned, [](bool assigned) {
              return !assigned;
            })) {
      details::error(
          "subgraph sample output has no authored source");
    }
  }

  // detach() is authored only as explicit writer/reader nodes plus a semantic
  // source connection. Project the cut-edge metadata only after sample
  // lowering has produced the compatibility topology.
  _detach.clear_materialized();
  for (auto const &info : _detach.authored_infos()) {
    auto const writer_node =
        _node_bundles.bundle(info.writer_bundle).single_concrete_node();
    std::optional<TopologyPortId> materialized_source;
    _topology.for_each_sample_edge([&](TopologyEdge const &edge) {
      if (edge.target != TopologyPortId{writer_node, 0}) return;
      if (materialized_source && *materialized_source != edge.source) {
        details::error("detach writer has more than one materialized source");
      }
      materialized_source = edge.source;
    });
    if (!materialized_source) {
      details::error("detach writer has no materialized source");
    }

    NodeBundlePortId const reader_port{
        info.reader_bundle, PortKind::sample, 0};
    auto const reader_channels =
        _node_bundles.sample_output_channels(reader_port);
    if (reader_channels.size() != 1 ||
        reader_channels.front() != info.reader_channel) {
      details::error(
          "detach reader no longer matches its authored sample channel");
    }
    auto const reader_descriptor =
        _node_bundles.resolve_sample_output(reader_port);
    if (reader_descriptor.endpoints.size() != 1) {
      details::error(
          "detach reader must have one completion topology output");
    }

    _detach.record_materialized_detached_source(
        *materialized_source,
        DetachedSamplePortInfo{
            .detach_id = info.detach_id,
            .original_source = *materialized_source,
            .writer_node = writer_node,
            .reader_output = reader_descriptor.endpoints.front(),
            .loop_extra_latency = info.loop_extra_latency,
        });
  }
}

void GraphBuilder::connect_sample_input(TopologyPortId target,
                                        MaterializedSamplePort source) {
  _connections.connect_sample_input(_topology, _identity, target, source.port);
}

void GraphBuilder::connect_sample_input(TopologyPortId target, SamplePortRef source) {
  connect_sample_input(target, materialize_sample_output(std::move(source)));
}

SamplePortRef GraphBuilder::normalize_sample_output(SamplePortRef source) {
  if (!source.graph_builder) {
    details::error("cannot normalize an empty sample output");
  }
  if (source.graph_builder != this) {
    details::error("cannot normalize a sample output from another builder");
  }
  if (!source.node_bundle_port) {
    if (source.channels.empty()) {
      details::error("sample output has no semantic channels");
    }
    if (source.channels.size() == 1) return source;

    switch (source.channel_type) {
    case ChannelTypeId::stereo: {
      auto pack = node<ChannelPack<stereo>>();
      auto const pack_handle = pack.node_bundle_handle();
      for (size_t channel = 0; channel < source.channels.size(); ++channel) {
        connect_sample_input_lowered(
            NodeBundlePortId{pack_handle, PortKind::sample, channel},
            SamplePortRef(
                *this, ChannelTypeId::mono,
                std::vector<SampleOutputChannelId>{source.channels[channel]}));
      }
      return static_cast<SamplePortRef>(pack);
    }
    case ChannelTypeId::mono: break;
    case ChannelTypeId::count: break;
    }
    details::error("invalid structural sample channel type");
  }

  auto const descriptor =
      _node_bundles.resolve_sample_output(*source.node_bundle_port);
  auto const &ports = descriptor.endpoints;
  if (ports.empty()) {
    details::error("NodeBundle sample output has no topology endpoint");
  }
  if (ports.size() == 1) return source;

  if (ports.size() != channel_count(descriptor.config.channel_layout.channel_type)) {
    details::error(
        "NodeBundle sample output does not match its declared channel layout");
  }
  switch (descriptor.config.channel_layout.channel_type) {
  case ChannelTypeId::mono:
    break;
  case ChannelTypeId::stereo: {
    auto pack = node<ChannelPack<stereo>>();
    auto const pack_handle = pack.node_bundle_handle();
    for (size_t channel = 0; channel < ports.size(); ++channel) {
      auto const target = _node_bundles.resolve_sample_input(
          NodeBundlePortId{pack_handle, PortKind::sample, channel});
      if (target.endpoints.size() != 1) {
        details::error("channel pack input must have one topology endpoint");
      }
      connect_sample_input(
          target.endpoints.front(),
          MaterializedSamplePort{
              .port = ports[channel]});
    }
    return static_cast<SamplePortRef>(pack);
  }
  case ChannelTypeId::count:
    break;
  }
  details::error("invalid channel type for NodeBundle sample output");
}

MaterializedSamplePort
GraphBuilder::materialize_sample_output(SamplePortRef source) {
  if (!source.graph_builder) {
    details::error("cannot materialize an empty sample output");
  }
  if (source.graph_builder != this) {
    details::error("cannot materialize a sample output from another builder");
  }
  if (source.graph_input_port) {
    return MaterializedSamplePort{
        .port = source.graph_input_port->topology_port()};
  }
  if (source.scope_boundary_port) {
    return MaterializedSamplePort{
        .port = source.scope_boundary_port->topology_port()};
  }
  if (!source.node_bundle_port && source.channels.size() == 1) {
    auto const channel = source.channels.front();
    auto const logical =
        NodeBundlePortId{channel.bundle, PortKind::sample, channel.port};
    auto const descriptor = _node_bundles.resolve_sample_output(logical);
    auto const physical_channels =
        channel_count(descriptor.config.channel_layout.channel_type);
    if (channel.channel >= physical_channels) {
      details::error("sample output channel is out of bounds");
    }
    if (descriptor.endpoints.size() == physical_channels) {
      return MaterializedSamplePort{
          .port = descriptor.endpoints[channel.channel]};
    }
    if (descriptor.endpoints.size() == 1) {
      if (physical_channels == 1) {
        return MaterializedSamplePort{.port = descriptor.endpoints.front()};
      }
      switch (descriptor.config.channel_layout.channel_type) {
      case ChannelTypeId::stereo: {
        auto unpack = node<ChannelUnpack<stereo>>();
        connect_sample_input_lowered(
            NodeBundlePortId{
                unpack.node_bundle_handle(), PortKind::sample, 0},
            SamplePortRef(*this, logical));
        return materialize_sample_output(
            static_cast<SamplePortRef>(unpack[channel.channel]));
      }
      case ChannelTypeId::mono: break;
      case ChannelTypeId::count: break;
      }
    }
    details::error(
        "sample output channel cannot be projected onto builder topology");
  }

  source = normalize_sample_output(std::move(source));
  if (!source.node_bundle_port) {
    details::error("SamplePortRef has no logical sample-output address");
  }
  auto const descriptor =
      _node_bundles.resolve_sample_output(*source.node_bundle_port);
  if (descriptor.endpoints.size() != 1) {
    details::error(
        "normalized sample output does not have exactly one topology endpoint");
  }
  return MaterializedSamplePort{
      .port = descriptor.endpoints.front()};
}

void GraphBuilder::connect_sample_input(NodeBundlePortId target,
                                        SamplePortRef source) {
  record_authored_sample_connection(target, source);
}

void GraphBuilder::connect_sample_input_lowered(NodeBundlePortId target,
                                                SamplePortRef source) {
  if (target.port_kind != PortKind::sample) {
    details::error("attempted to connect a sample source to an event NodeBundle port");
  }
  auto const descriptor = _node_bundles.resolve_sample_input(target);
  auto const &ports = descriptor.endpoints;
  if (ports.size() <= 1) {
    for (auto const port : ports) connect_sample_input(port, source);
    return;
  }
  if (!source.graph_builder) {
    details::error("cannot connect an empty sample output");
  }
  if (source.graph_builder != this) {
    details::error("cannot connect a sample output from another builder");
  }
  if (ports.size() !=
      channel_count(descriptor.config.channel_layout.channel_type)) {
    details::error(
        "tiled NodeBundle input does not match its declared channel layout");
  }

  if (source.channel_type == ChannelTypeId::mono) {
    auto const materialized = materialize_sample_output(source);
    for (auto const port : ports) connect_sample_input(port, materialized);
    return;
  }

  if (source.channel_type != descriptor.config.channel_layout.channel_type) {
    details::error("sample source and tiled input channel layouts do not match");
  }
  if (source.channels.size() != ports.size()) {
    details::error(
        "sample source channel count does not match tiled input channel count");
  }

  if (source.node_bundle_port) {
    auto const source_descriptor =
        _node_bundles.resolve_sample_output(*source.node_bundle_port);
    if (source_descriptor.config.channel_layout.channel_type
        != source.channel_type) {
      details::error(
          "sample source semantic type does not match its NodeBundle port");
    }
    auto const &source_ports = source_descriptor.endpoints;
    if (source_ports.size() == ports.size()) {
      for (size_t channel = 0; channel < ports.size(); ++channel) {
        connect_sample_input(
            ports[channel],
            MaterializedSamplePort{.port = source_ports[channel]});
      }
      return;
    }
    if (source_ports.size() != 1) {
      details::error(
          "sample source cannot be projected onto the tiled input channels");
    }

    switch (source.channel_type) {
    case ChannelTypeId::stereo: {
      auto unpack = node<ChannelUnpack<stereo>>();
      connect_sample_input_lowered(
          NodeBundlePortId{
              unpack.node_bundle_handle(), PortKind::sample, 0},
          source);
      for (size_t channel = 0; channel < ports.size(); ++channel) {
        connect_sample_input(ports[channel], unpack[channel]);
      }
      return;
    }
    case ChannelTypeId::mono: break;
    case ChannelTypeId::count: break;
    }
    details::error("invalid channel type for tiled NodeBundle sample input");
  }

  for (size_t channel = 0; channel < ports.size(); ++channel) {
    connect_sample_input(
        ports[channel],
        SamplePortRef(
            *this, ChannelTypeId::mono,
            std::vector<SampleOutputChannelId>{source.channels[channel]}));
  }
}

void GraphBuilder::connect_sample_input(
    NodeBundlePortId target, std::span<SamplePortRef const> sources) {
  record_authored_sample_connection(target, sources);
}

void GraphBuilder::connect_sample_input_lowered(
    NodeBundlePortId target, std::span<SamplePortRef const> sources) {
  if (target.port_kind != PortKind::sample) {
    details::error("attempted to connect sample channels to an event NodeBundle port");
  }
  auto const descriptor = _node_bundles.resolve_sample_input(target);
  auto const &ports = descriptor.endpoints;
  if (ports.size() == 1) {
    if (sources.size() !=
        channel_count(descriptor.config.channel_layout.channel_type)) {
      details::error(
          "sample channel source count does not match NodeBundle port layout");
    }
    switch (descriptor.config.channel_layout.channel_type) {
    case ChannelTypeId::mono:
      connect_sample_input(ports.front(), sources.front());
      return;
    case ChannelTypeId::stereo: {
      auto pack = node<ChannelPack<stereo>>();
      auto const pack_handle = pack.node_bundle_handle();
      for (size_t channel = 0; channel < sources.size(); ++channel) {
        connect_sample_input_lowered(
            NodeBundlePortId{pack_handle, PortKind::sample, channel},
            sources[channel]);
      }
      connect_sample_input(
          ports.front(), static_cast<SamplePortRef>(pack));
      return;
    }
    case ChannelTypeId::count:
      break;
    }
    details::error("invalid channel type for NodeBundle sample input");
  }
  if (sources.size() != ports.size()) {
    details::error("tiled NodeBundle input has an unexpected channel count");
  }
  for (size_t channel = 0; channel < ports.size(); ++channel) {
    connect_sample_input(ports[channel], sources[channel]);
  }
}

void GraphBuilder::connect_event_input(TopologyPortId target, EventPortRef source) {
  _connections.connect_event_input(_topology, _public_ports.event_inputs(_node_bundles),
                                   _identity, target, source);
}

void GraphBuilder::connect_event_input(NodeBundlePortId target, EventPortRef source) {
  record_authored_event_connection(target, source);
  connect_event_input_lowered(target, std::move(source));
}

void GraphBuilder::connect_event_input_lowered(
    NodeBundlePortId target, EventPortRef source) {
  if (target.port_kind != PortKind::event) {
    details::error("attempted to connect an event source to a sample NodeBundle port");
  }
  auto const descriptor = _node_bundles.resolve_event_input(target);
  for (auto const port : descriptor.endpoints) connect_event_input(port, source);
}

void GraphBuilder::mark_runtime_filled_sample_input(TopologyPortId target) {
  _connections.mark_runtime_filled_sample_input(target);
  if (target.node >= _topology.node_count()) return;
  for (auto const channel :
       _node_bundles.sample_input_channels_for_topology_port(target)) {
    _connections.mark_runtime_filled_sample_input(channel);
  }
}

void GraphBuilder::mark_runtime_filled_sample_input(NodeBundlePortId target) {
  for (auto const channel : _node_bundles.sample_input_channels(target)) {
    _connections.mark_runtime_filled_sample_input(channel);
  }
  // Runtime-filled ports still use a topology projection in the current
  // finalizer; this is independent of authored sample connectivity.
  auto const descriptor = _node_bundles.resolve_sample_input(target);
  for (auto const port : descriptor.endpoints) {
    _connections.mark_runtime_filled_sample_input(port);
  }
}

void GraphBuilder::mark_runtime_filled_event_input(TopologyPortId target) {
  _connections.mark_runtime_filled_event_input(target);
}

void GraphBuilder::mark_runtime_filled_event_input(NodeBundlePortId target) {
  auto const descriptor = _node_bundles.resolve_event_input(target);
  for (auto const port : descriptor.endpoints) mark_runtime_filled_event_input(port);
}

bool GraphBuilder::sample_input_is_connected(NodeBundlePortId target) const {
  if (target.port_kind != PortKind::sample) {
    details::error("attempted to inspect a sample connection on an event NodeBundle port");
  }
  auto const channels = _node_bundles.sample_input_channels(target);
  return std::ranges::any_of(channels, [&](auto channel) {
    return _connections.sample_input_is_connected(channel);
  });
}

bool GraphBuilder::event_input_is_connected(NodeBundlePortId target) const {
  if (target.port_kind != PortKind::event) {
    details::error("attempted to inspect an event connection on a sample NodeBundle port");
  }
  auto const descriptor = _node_bundles.resolve_event_input(target);
  bool connected = false;
  for (auto const port : descriptor.endpoints) {
    connected = connected || _connections.event_input_is_connected(port);
  }
  return connected;
}

std::vector<MaterializedSamplePort>
GraphBuilder::materialize_bundle_sample_output_channels(
    NodeBundlePortId source) {
  if (source.port_kind != PortKind::sample) {
    details::error(
        "attempted to read a sample output from an event NodeBundle port");
  }
  auto const descriptor = _node_bundles.resolve_sample_output(source);
  auto const &ports = descriptor.endpoints;
  if (ports.empty()) {
    details::error("NodeBundle sample output has no topology endpoint");
  }
  if (ports.size() > 1) {
    std::vector<MaterializedSamplePort> result;
    result.reserve(ports.size());
    for (auto const port : ports) {
      result.push_back(MaterializedSamplePort{.port = port});
    }
    return result;
  }

  auto const packed = SamplePortRef(*this, source);
  switch (descriptor.config.channel_layout.channel_type) {
  case ChannelTypeId::mono:
    return {materialize_sample_output(packed)};
  case ChannelTypeId::stereo: {
    auto unpack = node<ChannelUnpack<stereo>>();
    connect_sample_input_lowered(
        NodeBundlePortId{
            unpack.node_bundle_handle(), PortKind::sample, 0},
        packed);
    return {
        materialize_sample_output(static_cast<SamplePortRef>(unpack[0])),
        materialize_sample_output(static_cast<SamplePortRef>(unpack[1]))};
  }
  case ChannelTypeId::count:
    break;
  }
  details::error("invalid channel type for NodeBundle sample output");
}

void GraphBuilder::connect_sample_output(NodeBundlePortId source,
                                         NodeRef const &target) {
  auto const semantic_source = SamplePortRef(*this, source);
  std::vector<size_t> target_nodes;
  _node_bundles.bundle(target.node_bundle_handle()).for_each_topology_node(
      [&](size_t node) { target_nodes.push_back(node); });
  if (target_nodes.size() != 1) {
    details::error("a graph-service sink must contain exactly one concrete node");
  }
  auto const target_node = target_nodes.front();
  auto const &inputs = _topology.concrete_node(target_node).inputs();
  if (semantic_source.channels.size() != inputs.size()) {
    details::error("NodeBundle output does not match graph-service sink channel count");
  }
  for (size_t channel = 0; channel < inputs.size(); ++channel) {
    record_authored_sample_connection(
        NodeBundlePortId{
            target.node_bundle_handle(), PortKind::sample, channel},
        semantic_source.select_channel(channel));
  }
}

EventPortRef GraphBuilder::event_output(NodeBundlePortId source) const {
  if (source.port_kind != PortKind::event) {
    details::error("attempted to read an event output from a sample NodeBundle port");
  }
  auto const descriptor = _node_bundles.resolve_event_output(source);
  auto const &ports = descriptor.endpoints;
  if (ports.empty()) details::error("event output has no topology endpoints");

  auto &builder = const_cast<GraphBuilder &>(*this);
  auto const type = descriptor.config.type;
  auto semantic_sources = _node_bundles.event_output_ports(source);
  if (ports.size() == 1) {
    return EventPortRef(
        builder, type, std::move(semantic_sources), ports.front());
  }
  if (ports.size() != semantic_sources.size()) {
    details::error(
        "event output topology projection does not match its semantic sources");
  }

  auto merge = builder.node<EventConcatenation>(ports.size(), type);
  for (size_t index = 0; index < ports.size(); ++index) {
    auto const port = ports[index];
    builder.connect_event_input_lowered(
        NodeBundlePortId{
            merge.node_bundle_handle(), PortKind::event, index},
        EventPortRef(builder, port.node, port.port));
  }

  NodeBundlePortId const merged_output{
      merge.node_bundle_handle(), PortKind::event, 0};
  auto const merged_descriptor =
      _node_bundles.resolve_event_output(merged_output);
  if (merged_descriptor.endpoints.size() != 1) {
    details::error(
        "event concatenation output must have one topology endpoint");
  }
  return EventPortRef(
      builder, type, std::move(semantic_sources),
      merged_descriptor.endpoints.front());
}

size_t GraphBuilder::sample_port_index(NodeBundleHandle handle, bool inputs,
                                       std::string_view name) const {
  auto const &bundle = _node_bundles.bundle(handle);
  return inputs ? bundle.sample_input_index(name) : bundle.sample_output_index(name);
}

size_t GraphBuilder::event_port_index(NodeBundleHandle handle, bool inputs,
                                      std::string_view name) const {
  auto const &bundle = _node_bundles.bundle(handle);
  return inputs ? bundle.event_input_index(name) : bundle.event_output_index(name);
}

SamplePortRef
GraphBuilder::lift_to_sample_port(SamplePortRef const &sample_port) {
  if (sample_port.graph_builder != this) {
    details::error("builder " + _identity.value + ": sample port " +
                   sample_port.to_string() + " belongs to another builder");
  }
  return sample_port;
}

SamplePortRef GraphBuilder::lift_to_sample_port(SamplePortRef &&sample_port) {
  if (sample_port.graph_builder != this) {
    details::error("builder " + _identity.value + ": sample port " +
                   sample_port.to_string() + " belongs to another builder");
  }
  return std::move(sample_port);
}

SamplePortRef GraphBuilder::lift_to_sample_port(NamedRef const &ref) {
  return std::visit(
      [&](auto const &v) -> SamplePortRef {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::same_as<T, EventPortRef>) {
          details::error(
              "builder " + _identity.value +
              ": expected sample/sample-port value, got EventPortRef");
        } else if constexpr (std::same_as<T, SamplePortRef>) {
          return lift_to_sample_port(v);
        } else {
          return lift_to_sample_port(v);
        }
      },
      ref.value);
}

GraphIntrospectionMetadata
GraphBuilder::build_metadata(size_t detach_id_offset) const {
  auto completed = completed_sample_builder();
  return GraphBuilderFinalizer::build_metadata(
      completed._identity, completed._topology, completed._node_bundles,
      completed._virtual_nodes, completed._connections,
      detach_id_offset);
}

GraphBuilder::RootNodeBuildResult
GraphBuilder::build_root_node(size_t detach_id_offset) const {
  auto completed = completed_sample_builder();
  return GraphBuilderFinalizer::build_root_node(
      completed._identity, completed._topology, completed._node_bundles,
      completed._virtual_nodes, completed._connections,
      completed._public_ports, completed._detach, detach_id_offset);
}

GraphBuilder::RootNodeBuildResult
GraphBuilder::build_execution_root_node(size_t detach_id_offset) const {
  GraphBuilder execution_builder;
  execution_builder.embed_subgraph(*this);
  execution_builder.outputs({});
  return execution_builder.build_root_node(detach_id_offset);
}

} // namespace iv
