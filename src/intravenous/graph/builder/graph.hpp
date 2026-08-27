#pragma once

#include <intravenous/graph/builder/embedder.hpp>

#include <algorithm>
#include <ranges>

namespace iv {

constexpr SamplePortRef::SamplePortRef(
    GraphBuilder& builder, ChannelTypeId type,
    std::vector<SampleOutputChannelId> channels_)
    : graph_builder(&builder), channel_type(type), channels(std::move(channels_)) {
  if (channels.size() != channel_count(channel_type))
    details::error(
        "sample expression channel count does not match its semantic channel type");
  for (auto channel : channels) {
    auto config = builder._node_bundles.resolve_sample_output(
        {channel.bundle, PortKind::sample, channel.port}).config;
    if (channel.channel >= channel_count(config.channel_layout.channel_type))
      details::error("sample expression channel is out of bounds");
  }
}

constexpr SamplePortRef SamplePortRef::_clone_handle() const { return *this; }

constexpr SamplePortRef SamplePortRef::select_channel(size_t channel) const {
  if (!graph_builder)
    details::error("attempted to select a channel from an empty sample port");
  if (channel >= channels.size())
    details::error("sample channel ordinal is out of bounds");
  return SamplePortRef(
      *graph_builder, ChannelTypeId::mono, {channels[channel]});
}

inline std::string SamplePortRef::to_string() const {
  if (!graph_builder) return "empty sample port";
  if (auto logical = graph_builder->_node_bundles.sample_output_port_for_channels(
          channel_type, channels))
    return "sample output " + std::to_string(logical->port_ordinal) +
        " of node bundle " + std::to_string(logical->node_bundle_handle);
  return "structural sample expression with " +
      std::to_string(channels.size()) + " channel(s) in builder " +
      graph_builder->_identity.value;
}

constexpr void SamplePortRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const {
  if (graph_builder)
    graph_builder->annotate_sample_port_source_info(
        *this, id, file, begin, end);
}

constexpr EventPortRef::EventPortRef(
    GraphBuilder& builder, NodeBundlePortId port)
    : graph_builder(&builder) {
  if (port.port_kind != PortKind::event)
    details::error(
        "attempted to create an EventPortRef from a sample NodeBundle port");
  type = builder._node_bundles.resolve_event_output(port).config.type;
  sources = builder._node_bundles.event_output_ports(port);
}

constexpr EventPortRef::EventPortRef(
    GraphBuilder& builder, EventTypeId type_,
    std::vector<EventOutputPortId> sources_)
    : graph_builder(&builder), type(type_), sources(std::move(sources_)) {
  if (sources.empty()) details::error("event expression has no semantic sources");
  for (auto source : sources) {
    auto config = builder._node_bundles.resolve_event_output(
        {source.bundle, PortKind::event, source.port}).config;
    if (config.type != type)
      details::error(
          "event expression source does not match its semantic event type");
  }
}

inline std::string EventPortRef::to_string() const {
  if (!graph_builder) return "empty event";
  if (sources.size() == 1)
    return "event output " + std::to_string(sources.front().port) +
        " of node bundle " + std::to_string(sources.front().bundle);
  return "structural event expression with " +
      std::to_string(sources.size()) + " source(s) in builder " +
      graph_builder->_identity.value;
}

constexpr void EventPortRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const {
  if (graph_builder)
    graph_builder->annotate_event_port_source_info(
        *this, id, file, begin, end);
}

constexpr GraphBuilder GraphBuilder::derive_nested_builder() {
  return GraphBuilder(
      GraphBuilderIdentity(_identity.child_id(_node_bundles.size())));
}

constexpr PublicSampleInputRef GraphBuilder::input() {
  return input(Sample{0.0f});
}

constexpr PublicSampleInputRef GraphBuilder::input(
    Sample value, std::optional<Sample> min, std::optional<Sample> max) {
  return PublicSampleInputRef(
      _public_ports.add_sample_input(
          *this, _node_bundles, {}, value, min, max));
}

constexpr PublicEventInputRef GraphBuilder::event_input_named(
    std::string_view name, EventTypeId type) {
  return PublicEventInputRef(
      _public_ports.add_event_input(*this, _node_bundles, name, type));
}

constexpr PublicEventInputRef GraphBuilder::event_input(EventTypeId type) {
  return PublicEventInputRef(
      _public_ports.add_event_input(*this, _node_bundles, {}, type));
}

constexpr void GraphBuilder::annotate_public_sample_input_source_info(
    PublicSampleInputRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || ref.port.graph_builder != this) return;
  auto logical = _node_bundles.sample_output_port_for_channels(
      ref.port.channel_type, ref.port.channels);
  if (!logical)
    details::error("PublicSampleInputRef has no logical boundary port");
  if (logical->node_bundle_handle == _public_ports.boundary_handle()) {
    _public_ports.annotate_sample_input_source_info(
        logical->port_ordinal, id, file, begin, end);
    return;
  }
  auto& boundary = _node_bundles.bundle(logical->node_bundle_handle);
  if (!boundary.is_boundary() ||
      logical->port_ordinal >= boundary.boundary_sample_inputs().size())
    details::error(
        "PublicSampleInputRef does not belong to a valid boundary input");
  SourceInfo info{
      .declaration_identity = std::string(id),
      .span = {
          .file_path = std::string(file), .begin = begin, .end = end}};
  auto& infos = boundary.source_annotations().infos;
  if (!std::ranges::contains(infos, info)) infos.push_back(std::move(info));
}

constexpr void PublicSampleInputRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const {
  if (port.graph_builder)
    port.graph_builder->annotate_public_sample_input_source_info(
        *this, id, file, begin, end);
}

constexpr void GraphBuilder::annotate_public_event_input_source_info(
    PublicEventInputRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || ref.port.graph_builder != this) return;
  if (ref.port.sources.size() != 1)
    details::error("PublicEventInputRef has no unique logical boundary port");
  auto source = ref.port.sources.front();
  if (source.bundle == _public_ports.boundary_handle()) {
    _public_ports.annotate_event_input_source_info(
        source.port, id, file, begin, end);
    return;
  }
  auto& boundary = _node_bundles.bundle(source.bundle);
  if (!boundary.is_boundary() ||
      source.port >= boundary.boundary_event_inputs().size())
    details::error(
        "PublicEventInputRef does not belong to a valid boundary input");
  SourceInfo info{
      .declaration_identity = std::string(id),
      .span = {
          .file_path = std::string(file), .begin = begin, .end = end}};
  auto& infos = boundary.source_annotations().infos;
  if (!std::ranges::contains(infos, info)) infos.push_back(std::move(info));
}

constexpr void PublicEventInputRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const {
  if (port.graph_builder)
    port.graph_builder->annotate_public_event_input_source_info(
        *this, id, file, begin, end);
}

constexpr void GraphBuilder::annotate_sample_port_source_info(
    SamplePortRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || ref.graph_builder != this) return;
  SourceInfo info{
      .declaration_identity = std::string(id),
      .span = {.file_path = std::string(file), .begin = begin, .end = end}};
  _virtual_nodes.attach_sample_output(
      _node_bundles, ref.channel_type, ref.channels, id, info);
}

constexpr void GraphBuilder::annotate_event_port_source_info(
    EventPortRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || ref.graph_builder != this) return;
  SourceInfo info{
      .declaration_identity = std::string(id),
      .span = {.file_path = std::string(file), .begin = begin, .end = end}};
  _virtual_nodes.attach_event_output(
      _node_bundles, ref.type, ref.sources, id, info);
}

constexpr void GraphBuilder::annotate_public_sample_output_source_info(
    std::span<SourceInfo const> infos) {
  for (size_t i = 0; i < infos.size(); ++i)
    _public_ports.annotate_sample_output_source_info(i, infos[i]);
}

constexpr void GraphBuilder::annotate_public_event_output_source_info(
    std::span<SourceInfo const> infos) {
  for (size_t i = 0; i < infos.size(); ++i)
    _public_ports.annotate_event_output_source_info(i, infos[i]);
}

constexpr void GraphBuilder::annotate_public_sample_output_source_info(
    size_t ordinal, SourceInfo info) {
  _public_ports.annotate_sample_output_source_info(
      ordinal, std::move(info));
}

constexpr void GraphBuilder::annotate_public_event_output_source_info(
    size_t ordinal, SourceInfo info) {
  _public_ports.annotate_event_output_source_info(
      ordinal, std::move(info));
}

constexpr NodeRef GraphBuilder::embed_subgraph(
    GraphBuilder const& child, std::string_view kind) {
  if (!child._public_ports.sample_outputs_defined())
    details::error(
        "builder " + child._identity.value +
        ": g.outputs(...) must be called before insertion");
  auto const begin = _node_bundles.size();
  auto const offset = GraphBuilderChildEmbedder::embed(
      _node_bundles, _connections, _detach, _virtual_nodes,
      child._public_ports, child._node_bundles, child._connections,
      child._detach, child._virtual_nodes);
  IV_ASSERT(offset == begin, "embedded child bundle offset changed unexpectedly");
  auto const boundary = offset + child._public_ports.boundary_handle();
  auto const count = child._node_bundles.size();
  return NodeRef(
      *this, _node_bundles.append_subgraph(boundary, begin, count, kind));
}

constexpr void GraphBuilder::event_outputs(
    std::span<EventOutputRefConfig const> refs) {
  _public_ports.define_event_outputs(
      *this, _node_bundles, _identity, refs);
}

consteval void GraphBuilder::outputs(
    std::span<OutputRefConfig const> refs) {
  _public_ports.define_sample_outputs(
      *this, _node_bundles, _identity, refs);
}

constexpr GraphBuilder::VacantInputs GraphBuilder::vacant_inputs() const {
  return _connections.collect_vacant_inputs(_node_bundles, _virtual_nodes);
}

constexpr GraphBuilder::VirtualInputs GraphBuilder::virtual_inputs() const {
  return _connections.collect_virtual_inputs(_node_bundles, _virtual_nodes);
}

constexpr GraphBuilder::VirtualSampleInputFamilies
GraphBuilder::virtual_sample_input_families() const {
  return _connections.collect_virtual_sample_input_families(
      _node_bundles, _virtual_nodes);
}

constexpr GraphBuilder::VirtualOutputs GraphBuilder::virtual_outputs() const {
  return _connections.collect_virtual_outputs(_node_bundles, _virtual_nodes);
}

constexpr GraphBuilder::VirtualSampleOutputFamilies
GraphBuilder::virtual_sample_output_families() const {
  return _connections.collect_virtual_sample_output_families(
      _node_bundles, _virtual_nodes);
}

constexpr GraphBuilder::VirtualPorts GraphBuilder::virtual_ports() const {
  return _virtual_nodes.ports(_node_bundles);
}

constexpr void GraphBuilder::record_authored_sample_connection(
    NodeBundlePortId target, std::span<SamplePortRef const> sources) {
  auto targets = _node_bundles.sample_input_channels(target);
  auto type = _node_bundles.resolve_sample_input(target)
                  .config.channel_layout.channel_type;
  if (sources.size() != targets.size())
    details::error(
        "sample channel source count does not match NodeBundle port layout");
  std::vector<SampleOutputChannelId> channels;
  for (auto const& source : sources) {
    if (source.graph_builder != this ||
        source.channel_type != ChannelTypeId::mono ||
        source.channels.size() != 1)
      details::error(
          "channel-wise sample connection requires scalar sources");
    channels.push_back(source.channels.front());
  }
  _connections.record_authored_sample_connection(
      {type, std::move(channels), type, std::move(targets)});
}

constexpr void GraphBuilder::record_authored_event_connection(
    NodeBundlePortId target, EventPortRef const& source) {
  if (target.port_kind != PortKind::event ||
      source.graph_builder != this || source.sources.empty())
    details::error("invalid authored event connection");
  auto target_type = _node_bundles.resolve_event_input(target).config.type;
  _connections.record_authored_event_connection({
      source.type, source.sources, target_type,
      _node_bundles.event_input_ports(target)});
}

constexpr void GraphBuilder::connect_sample_input(
    NodeBundlePortId target, std::span<SamplePortRef const> sources) {
  record_authored_sample_connection(target, sources);
}

constexpr void GraphBuilder::connect_event_input(
    NodeBundlePortId target, EventPortRef source) {
  record_authored_event_connection(target, source);
}

constexpr bool GraphBuilder::sample_input_is_connected(
    NodeBundlePortId target) const {
  if (target.port_kind != PortKind::sample)
    details::error("sample connectivity requested for event port");
  auto channels = _node_bundles.sample_input_channels(target);
  return std::ranges::any_of(channels, [&](auto channel) {
    return _connections.sample_input_is_connected(channel);
  });
}

constexpr bool GraphBuilder::event_input_is_connected(
    NodeBundlePortId target) const {
  if (target.port_kind != PortKind::event)
    details::error("event connectivity requested for sample port");
  auto ports = _node_bundles.event_input_ports(target);
  return std::ranges::any_of(ports, [&](auto port) {
    return _connections.event_input_is_connected(port);
  });
}

constexpr void GraphBuilder::connect_sample_output(
    NodeBundlePortId source, NodeRef const& target) {
  auto semantic = SamplePortRef(*this, source);
  auto const& sink = _node_bundles.bundle(target.node_bundle_handle());
  if (!sink.is_concrete())
    details::error("a graph-service sink must be one concrete node bundle");
  if (semantic.channels.size() != sink.sample_input_count())
    details::error(
        "NodeBundle output does not match graph-service sink channel count");
  for (size_t i = 0; i < sink.sample_input_count(); ++i)
    record_authored_sample_connection(
        {target.node_bundle_handle(), PortKind::sample, i},
        semantic.select_channel(i));
}

constexpr EventPortRef GraphBuilder::event_output(
    NodeBundlePortId source) const {
  if (source.port_kind != PortKind::event)
    details::error(
        "attempted to read an event output from a sample NodeBundle port");
  return EventPortRef(const_cast<GraphBuilder&>(*this), source);
}

constexpr size_t GraphBuilder::sample_port_index(
    NodeBundleHandle handle, bool inputs, std::string_view name) const {
  auto const& candidate = _node_bundles.bundle(handle);
  auto const count = inputs
      ? candidate.sample_input_count()
      : candidate.sample_output_count();
  std::optional<size_t> match;
  for (size_t i = 0; i < count; ++i) {
    auto const port_name = inputs
        ? _node_bundles.resolve_sample_input(
              {handle, PortKind::sample, i}).config.name
        : _node_bundles.resolve_sample_output(
              {handle, PortKind::sample, i}).config.name;
    if (port_name != name) continue;
    if (match)
      details::error(
          "NodeBundle port name '" + std::string(name) + "' is ambiguous");
    match = i;
  }
  if (!match)
    details::error(
        "NodeBundle port name '" + std::string(name) + "' does not exist");
  return *match;
}

constexpr size_t GraphBuilder::event_port_index(
    NodeBundleHandle handle, bool inputs, std::string_view name) const {
  auto const& candidate = _node_bundles.bundle(handle);
  auto const count = inputs
      ? candidate.event_input_count()
      : candidate.event_output_count();
  std::optional<size_t> match;
  for (size_t i = 0; i < count; ++i) {
    auto const port_name = inputs
        ? _node_bundles.resolve_event_input(
              {handle, PortKind::event, i}).config.name
        : _node_bundles.resolve_event_output(
              {handle, PortKind::event, i}).config.name;
    if (port_name != name) continue;
    if (match)
      details::error(
          "NodeBundle port name '" + std::string(name) + "' is ambiguous");
    match = i;
  }
  if (!match)
    details::error(
        "NodeBundle port name '" + std::string(name) + "' does not exist");
  return *match;
}

} // namespace iv
