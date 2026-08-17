#include <intravenous/graph/builder.h>

#include <intravenous/basic_nodes/routing.h>
#include <intravenous/graph/builder/embedder.h>

namespace iv {
namespace {
std::vector<MaterializedSamplePort> bundle_sample_output_channels(
    GraphBuilder &builder, GraphBuilderNodeBundles const &node_bundles,
    NodeBundlePortId source) {
  if (source.port_kind != PortKind::sample) {
    details::error("attempted to read a sample output from an event NodeBundle port");
  }
  auto const descriptor = node_bundles.resolve_sample_output(source);
  auto const &ports = descriptor.endpoints;
  if (ports.empty()) {
    details::error("NodeBundle sample output has no topology endpoint");
  }
  if (ports.size() > 1) {
    std::vector<MaterializedSamplePort> result;
    result.reserve(ports.size());
    for (auto const port : ports) {
      result.push_back(MaterializedSamplePort{
          .port = port});
    }
    return result;
  }
  auto const packed = SamplePortRef(builder, source);
  switch (descriptor.config.channel_layout.channel_type) {
  case ChannelTypeId::mono:
    return {builder.materialize_sample_output(packed)};
  case ChannelTypeId::stereo: {
    auto unpack = builder.node<ChannelUnpack<stereo>>();
    unpack.connect_input(0, packed);
    return {
        builder.materialize_sample_output(static_cast<SamplePortRef>(unpack[0])),
        builder.materialize_sample_output(static_cast<SamplePortRef>(unpack[1]))};
  }
  case ChannelTypeId::count: break;
  }
  details::error("invalid channel type for NodeBundle sample output");
}

SamplePortRef pack_sample_channels(GraphBuilder &builder, ChannelLayout layout,
                                   std::span<SamplePortRef const> sources) {
  if (sources.size() != channel_count(layout.channel_type)) {
    details::error("sample channel source count does not match NodeBundle port layout");
  }
  switch (layout.channel_type) {
  case ChannelTypeId::mono:
    return sources.front();
  case ChannelTypeId::stereo: {
    auto pack = builder.node<ChannelPack<stereo>>();
    for (size_t channel = 0; channel < sources.size(); ++channel)
      pack.connect_input(channel, sources[channel]);
    return static_cast<SamplePortRef>(pack);
  }
  case ChannelTypeId::count:
    break;
  }
  details::error("invalid channel type for NodeBundle sample input");
}
} // namespace
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
  if (graph_input.port_ordinal >= graph_builder_._public_ports.sample_inputs().size()) {
    details::error(
        "graph input port " + std::to_string(graph_input.port_ordinal) +
        " is out of bounds in builder " + graph_builder_._identity.value);
  }
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
  if (graph_input.port_ordinal >= graph_builder_._public_ports.event_inputs().size()) {
    details::error("graph event input port " +
                   std::to_string(graph_input.port_ordinal) +
                   " is out of bounds in builder " +
                   graph_builder_._identity.value);
  }
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
  (void)graph_builder_._topology.scope_boundary_event_output(scope_boundary);
}

SamplePortRef::SamplePortRef(GraphBuilder &graph_builder_,
                             NodeBundlePortId bundle_port)
    : graph_builder(&graph_builder_), node_bundle_port(bundle_port) {
  if (bundle_port.port_kind != PortKind::sample) {
    details::error("attempted to create a SamplePortRef from an event NodeBundle port");
  }
  (void)graph_builder_._node_bundles.resolve_sample_output(bundle_port);
}

SamplePortRef::operator TopologyPortId() const {
  if (graph_input_port) return graph_input_port->topology_port();
  if (scope_boundary_port) return scope_boundary_port->topology_port();
  details::error(
      "a logical node output SamplePortRef must be materialized by GraphBuilder");
}

SamplePortRef SamplePortRef::_clone_handle() const {
  if (!graph_builder) {
    return SamplePortRef{};
  }
  if (node_bundle_port) {
    return SamplePortRef(*graph_builder, *node_bundle_port);
  }
  if (graph_input_port) {
    return SamplePortRef(*graph_builder, *graph_input_port);
  }
  if (scope_boundary_port) {
    return SamplePortRef(*graph_builder, *scope_boundary_port);
  }
  details::error("invalid SamplePortRef address");
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
  if (node_bundle_port) {
    return "sample output " + std::to_string(node_bundle_port->port_ordinal) +
           " of node bundle " + std::to_string(node_bundle_port->node_bundle_handle);
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
    : _identity(std::move(identity)) {}

GraphBuilder::GraphBuilder() : _identity(allocate_root_builder_id()) {}

GraphBuilder GraphBuilder::derive_nested_builder() {
  return GraphBuilder(
      GraphBuilderIdentity(_identity.child_id(_topology.node_count())));
}

bool GraphBuilder::inside_subgraph_scope() const { return _subgraphs.active(); }

ScopedSubgraph &GraphBuilder::current_scope() { return _subgraphs.current(); }

void GraphBuilder::define_scope_outputs(std::span<OutputRefConfig const> refs) {
  _subgraphs.define_sample_outputs(refs, *this, _topology, _identity);
}

void GraphBuilder::define_scope_event_outputs(
    std::span<EventOutputRefConfig const> refs) {
  _subgraphs.define_event_outputs(refs, *this, _topology, _identity,
                                  _public_ports.event_inputs());
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
      _public_ports.add_sample_input(*this, name, default_value, min, max));
}

PublicSampleInputRef GraphBuilder::input(Sample default_value,
                                         std::optional<Sample> min,
                                         std::optional<Sample> max) {
  if (inside_subgraph_scope()) {
    return PublicSampleInputRef(_subgraphs.add_scope_sample_input(
        *this, _topology, _node_bundles, {}, default_value, min, max, false));
  }
  return PublicSampleInputRef(
      _public_ports.add_sample_input(*this, {}, default_value, min, max));
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
  if (ref.port.scope_boundary_port) {
    _subgraphs.annotate_scope_input_source_info(
        *ref.port.scope_boundary_port,
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
  return PublicEventInputRef(_public_ports.add_event_input(*this, name, type));
}

PublicEventInputRef GraphBuilder::event_input(EventTypeId type) {
  if (inside_subgraph_scope()) {
    return PublicEventInputRef(
        _subgraphs.add_scope_event_input(*this, _topology, _node_bundles, {}, type, false));
  }
  return PublicEventInputRef(_public_ports.add_event_input(*this, {}, type));
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
        *ref.port.scope_boundary_port,
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

  size_t const subgraph_node = GraphBuilderChildEmbedder::embed(
      _topology, _node_bundles, _connections, _detach, _virtual_nodes,
      child._public_ports, child._topology, child._node_bundles,
      child._connections, child._detach, child._virtual_nodes);
  return NodeRef(*this,
                 _node_bundles.append_subgraph(_topology, subgraph_node));
}

void GraphBuilder::event_outputs(std::span<EventOutputRefConfig const> refs) {
  _public_ports.define_event_outputs(*this, _topology, _identity, refs);
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
  _public_ports.define_sample_outputs(*this, _topology, _identity, refs);
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
      *this, _topology, _identity,
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

  auto const resolved_source = materialize_sample_output(sample_port);
  TopologyPortId const source = resolved_source.port;
  if (_detach.reader_output_exists(source)) {
    return sample_port;
  }
  if (auto const *existing = _detach.info_for_source(source)) {
    if (existing->loop_extra_latency != loop_extra_latency) {
      details::error("builder " + _identity.value +
                     ": detach loop extra latency conflict on " +
                     sample_port.to_string());
    }
    TopologyPortId const reader = existing->reader_output;
    auto const reader_bundle =
        _node_bundles.bundle_for_concrete_node(reader.node);
    return SamplePortRef(
        *this, NodeBundlePortId{reader_bundle, PortKind::sample, reader.port});
  }
  if (loop_extra_latency < 1) {
    details::error("builder " + _identity.value +
                   ": detach loop extra latency must be at least 1");
  }

  size_t const detach_id = _detach.allocate_detach_id();
  auto writer = node<DetachWriterNode>(detach_id, loop_extra_latency);
  (void)writer;
  size_t const writer_node = _topology.node_count() - 1;
  connect_sample_input(TopologyPortId{writer_node, 0}, resolved_source);

  auto reader = node<DetachReaderNode>(detach_id, loop_extra_latency);
  SamplePortRef detached = static_cast<SamplePortRef>(reader);
  auto const detached_source = materialize_sample_output(detached);

  _detach.record_detached_source(source,
                                 DetachedSamplePortInfo{
                                     .detach_id = detach_id,
                                     .original_source = source,
                                     .writer_node = writer_node,
                                     .reader_output = detached_source.port,
                                     .loop_extra_latency = loop_extra_latency,
                                 });
  return detached;
}

GraphBuilder::VacantInputs GraphBuilder::vacant_inputs() const {
  return _connections.collect_vacant_inputs(_topology, _virtual_nodes);
}

GraphBuilder::VirtualInputs GraphBuilder::virtual_inputs() const {
  return _connections.collect_virtual_inputs(_topology, _virtual_nodes);
}

GraphBuilder::VirtualSampleInputFamilies
GraphBuilder::virtual_sample_input_families() const {
  return _connections.collect_virtual_sample_input_families(_node_bundles,
                                                            _virtual_nodes);
}

GraphBuilder::VirtualOutputs GraphBuilder::virtual_outputs() const {
  return _connections.collect_virtual_outputs(_topology, _virtual_nodes);
}

GraphBuilder::VirtualSampleOutputFamilies
GraphBuilder::virtual_sample_output_families() const {
  return _connections.collect_virtual_sample_output_families(_topology,
                                                             _node_bundles,
                                                             _virtual_nodes);
}

GraphBuilder::VirtualPorts GraphBuilder::virtual_ports() const {
  return _virtual_nodes.ports(_topology, _node_bundles);
}

GraphBuilderPublicSamplePortFamilies
GraphBuilder::public_sample_input_families() const {
  return _public_ports.sample_input_families();
}

bool GraphBuilder::public_sample_input_is_connected(size_t port_ordinal) const {
  auto const source =
      GraphInputPortId{PortKind::sample, port_ordinal}.topology_port();
  bool connected = false;
  _topology.for_each_sample_edge([&](TopologyEdge const &edge) {
    connected = connected || (edge.source == source);
  });
  return connected;
}

std::vector<GraphBuilderPublicEventInput>
GraphBuilder::public_event_inputs() const {
  return _public_ports.collected_event_inputs();
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
  return _public_ports.sample_output_families();
}

std::vector<GraphBuilderPublicEventOutput>
GraphBuilder::public_event_outputs() const {
  return _public_ports.collected_event_outputs();
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
  if (!source.node_bundle_port) return source;

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

void GraphBuilder::connect_sample_input(NodeBundlePortId target, SamplePortRef source) {
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

  // Graph and scope boundaries are mono today; preserve the existing
  // broadcast semantics for them and for ordinary mono bundle outputs.
  if (!source.node_bundle_port) {
    for (auto const port : ports) connect_sample_input(port, source);
    return;
  }

  auto const source_descriptor =
      _node_bundles.resolve_sample_output(*source.node_bundle_port);
  auto const source_channel_type =
      source_descriptor.config.channel_layout.channel_type;
  if (source_channel_type == ChannelTypeId::mono) {
    for (auto const port : ports) connect_sample_input(port, source);
    return;
  }
  if (source_channel_type != descriptor.config.channel_layout.channel_type) {
    details::error("sample source and tiled input channel layouts do not match");
  }

  auto const &source_ports = source_descriptor.endpoints;
  if (source_ports.size() == ports.size()) {
    for (size_t channel = 0; channel < ports.size(); ++channel) {
      connect_sample_input(
          ports[channel],
          MaterializedSamplePort{
              .port = source_ports[channel]});
    }
    return;
  }
  if (source_ports.size() != 1) {
    details::error(
        "sample source cannot be projected onto the tiled input channels");
  }

  switch (source_channel_type) {
  case ChannelTypeId::stereo: {
    auto unpack = node<ChannelUnpack<stereo>>();
    unpack.connect_input(0, source);
    for (size_t channel = 0; channel < ports.size(); ++channel) {
      connect_sample_input(ports[channel], unpack[channel]);
    }
    return;
  }
  case ChannelTypeId::mono:
    break;
  case ChannelTypeId::count:
    break;
  }
  details::error("invalid channel type for tiled NodeBundle sample input");
}

void GraphBuilder::connect_sample_input(
    NodeBundlePortId target, std::span<SamplePortRef const> sources) {
  if (target.port_kind != PortKind::sample) {
    details::error("attempted to connect sample channels to an event NodeBundle port");
  }
  auto const descriptor = _node_bundles.resolve_sample_input(target);
  auto const &ports = descriptor.endpoints;
  if (ports.size() == 1) {
    connect_sample_input(ports.front(),
                         pack_sample_channels(*this,
                                              descriptor.config.channel_layout,
                                              sources));
    return;
  }
  if (sources.size() != ports.size()) {
    details::error("tiled NodeBundle input has an unexpected channel count");
  }
  for (size_t channel = 0; channel < ports.size(); ++channel) {
    connect_sample_input(ports[channel], sources[channel]);
  }
}

void GraphBuilder::connect_event_input(TopologyPortId target, EventPortRef source) {
  _connections.connect_event_input(_topology, _public_ports.event_inputs(),
                                   _identity, target, source);
}

void GraphBuilder::connect_event_input(NodeBundlePortId target, EventPortRef source) {
  if (target.port_kind != PortKind::event) {
    details::error("attempted to connect an event source to a sample NodeBundle port");
  }
  auto const descriptor = _node_bundles.resolve_event_input(target);
  for (auto const port : descriptor.endpoints) connect_event_input(port, source);
}

void GraphBuilder::mark_runtime_filled_sample_input(TopologyPortId target) {
  _connections.mark_runtime_filled_sample_input(target);
}

void GraphBuilder::mark_runtime_filled_sample_input(NodeBundlePortId target) {
  auto const descriptor = _node_bundles.resolve_sample_input(target);
  for (auto const port : descriptor.endpoints) mark_runtime_filled_sample_input(port);
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
  auto const descriptor = _node_bundles.resolve_sample_input(target);
  bool connected = false;
  for (auto const port : descriptor.endpoints) {
    connected = connected || _connections.sample_input_is_connected(port);
  }
  return connected;
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

void GraphBuilder::connect_sample_output(NodeBundlePortId source,
                                         NodeRef const &target) {
  auto const channels = bundle_sample_output_channels(
      *this, _node_bundles, source);
  std::vector<size_t> target_nodes;
  _node_bundles.bundle(target.node_bundle_handle()).for_each_topology_node(
      [&](size_t node) { target_nodes.push_back(node); });
  if (target_nodes.size() != 1) {
    details::error("a graph-service sink must contain exactly one concrete node");
  }
  auto const target_node = target_nodes.front();
  auto const &inputs = _topology.concrete_node(target_node).inputs();
  if (channels.size() != inputs.size()) {
    details::error("NodeBundle output does not match graph-service sink channel count");
  }
  for (size_t channel = 0; channel < channels.size(); ++channel) {
    connect_sample_input(TopologyPortId{target_node, channel}, channels[channel]);
  }
}

EventPortRef GraphBuilder::event_output(NodeBundlePortId source) const {
  if (source.port_kind != PortKind::event) {
    details::error("attempted to read an event output from a sample NodeBundle port");
  }
  auto const descriptor = _node_bundles.resolve_event_output(source);
  auto const &ports = descriptor.endpoints;
  if (ports.empty()) details::error("event output has no topology endpoints");
  if (ports.size() == 1) {
    auto const port = ports.front();
    return EventPortRef(const_cast<GraphBuilder &>(*this), port.node, port.port);
  }
  auto const type = descriptor.config.type;
  auto merge = const_cast<GraphBuilder &>(*this).node<EventConcatenation>(ports.size(), type);
  for (size_t index = 0; index < ports.size(); ++index) {
    auto const port = ports[index];
    merge.connect_event_input(index,
        EventPortRef(const_cast<GraphBuilder &>(*this), port.node, port.port));
  }
  return merge.event_port(0);
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
  return GraphBuilderFinalizer::build_metadata(
      _identity, _topology, _node_bundles, _virtual_nodes, detach_id_offset);
}

GraphBuilder::RootNodeBuildResult
GraphBuilder::build_root_node(size_t detach_id_offset) const {
  return GraphBuilderFinalizer::build_root_node(
      _identity, _topology, _node_bundles, _virtual_nodes, _connections, _public_ports,
      _detach, detach_id_offset);
}

GraphBuilder::RootNodeBuildResult
GraphBuilder::build_execution_root_node(size_t detach_id_offset) const {
  GraphBuilder execution_builder;
  execution_builder.embed_subgraph(*this);
  execution_builder.outputs({});
  return execution_builder.build_root_node(detach_id_offset);
}

} // namespace iv
