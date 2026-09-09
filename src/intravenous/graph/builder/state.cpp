#include <intravenous/graph/builder/state.h>

#include <intravenous/graph/builder/embedder.hpp>

#include <algorithm>
#include <array>
#include <ranges>

namespace iv {
namespace {
std::vector<EventOutputRefConfig> make_event_output_configs(
    std::span<EventOutputRequest const> refs) {
  std::vector<EventOutputRefConfig> configs;
  configs.reserve(refs.size());
  for (auto const& ref : refs) {
    configs.push_back({
        .ref = ref.ref,
        .config = {.name = std::string(ref.name)},
    });
  }
  return configs;
}

std::vector<OutputRefConfig> make_sample_output_configs(
    std::span<SampleOutputRequest const> refs) {
  std::vector<OutputRefConfig> configs;
  configs.reserve(refs.size());
  for (auto const& ref : refs) {
    configs.push_back({
        .ref = ref.ref,
        .config = {
            .name = std::string(ref.name),
            .channel_layout = ref.channel_layout,
        },
        .public_member = {
            .family_name = std::string(ref.family_name),
            .channel_type = ref.family_channel_type,
            .whole_stream = ref.whole_stream,
        },
        .target_channel_ordinal = ref.targets_single_channel()
            ? std::optional{ref.target_channel_ordinal}
            : std::nullopt,
    });
  }
  return configs;
}
} // namespace

NodeBundleHandle GraphBuilderState::append_node_description(
    ReflectedNodeDescription description)
{
  return _node_bundles.append_concrete(
      GraphBuilderNodeBundles::make_concrete_node(std::move(description)));
}

NodeBundleHandle GraphBuilderState::append_tiled_node_description(
    ReflectedNodeDescription const& description, ChannelLayout layout)
{
  auto concrete = GraphBuilderNodeBundles::make_concrete_node(description);
  for (auto const& config : concrete.inputs()) {
    if (config.channel_layout.channel_type != ChannelTypeId::mono)
      details::error(
          "the tiled-node model requires fully mono concrete sample nodes");
  }
  for (auto const& config : concrete.outputs()) {
    if (config.channel_layout.channel_type != ChannelTypeId::mono)
      details::error(
          "the tiled-node model requires fully mono concrete sample nodes");
  }

  std::vector<NodeBundleHandle> members;
  members.reserve(channel_count(layout.channel_type));
  for (size_t channel = 0; channel < channel_count(layout.channel_type); ++channel)
    members.push_back(_node_bundles.append_concrete(concrete));
  return _node_bundles.append_tiled(members, layout);
}

SamplePortRef GraphBuilderPublicPorts::add_sample_input(
    GraphBuilderState& builder, GraphBuilderNodeBundles& bundles,
    std::string_view name, Sample value, std::optional<Sample> min,
    std::optional<Sample> max) {
  return add_sample_input(
      builder, bundles, name,
      {
          .channel_type = ChannelTypeId::mono,
          .sample_layout = SampleStreamLayout::planar,
      },
      value, min, max);
}

SamplePortRef GraphBuilderPublicPorts::add_sample_input(
    GraphBuilderState& builder, GraphBuilderNodeBundles& bundles,
    std::string_view name, ChannelLayout channel_layout, Sample value,
    std::optional<Sample> min, std::optional<Sample> max) {
  auto ordinal = bundles.bundle(_boundary).append_boundary_sample_input({
      .name = std::string(name),
      .channel_layout = channel_layout,
      .default_value = value,
      .min = min.value_or(-std::numeric_limits<Sample::storage>::infinity()),
      .max = max.value_or(std::numeric_limits<Sample::storage>::infinity())});
  _sample_input_source_infos.emplace_back();
  return SamplePortRef(
      builder.facade(), NodeBundlePortId{_boundary, PortKind::sample, ordinal});
}

EventPortRef GraphBuilderPublicPorts::add_event_input(
    GraphBuilderState& builder, GraphBuilderNodeBundles& bundles,
    std::string_view name, EventTypeId type) {
  auto ordinal = bundles.bundle(_boundary).append_boundary_event_input(
      {.name = std::string(name), .type = type});
  _event_input_source_infos.emplace_back();
  return EventPortRef(
      builder.facade(), NodeBundlePortId{_boundary, PortKind::event, ordinal});
}

SamplePortRef GraphBuilderState::make_sample_port(
    ChannelTypeId type,
    std::span<SampleOutputChannelId const> channels)
{
  if (channels.size() != channel_count(type))
    details::error(
        "sample expression channel count does not match its semantic channel type");
  for (auto channel : channels) {
    auto config = _node_bundles.resolve_sample_output(
        {channel.bundle, PortKind::sample, channel.port}).config;
    if (channel.channel >= channel_count(config.channel_layout.channel_type))
      details::error("sample expression channel is out of bounds");
  }
  auto const handle = _sample_port_expressions.size();
  _sample_port_expressions.push_back({
      .channel_type = type,
      .channels = {channels.begin(), channels.end()},
  });
  SamplePortRef result;
  result.graph_builder = &facade();
  result.channel_type = type;
  result.handle = handle;
  return result;
}

std::span<SampleOutputChannelId const>
GraphBuilderState::sample_port_channels(SamplePortRef const& ref) const
{
  if (ref.graph_builder != &facade()
      || ref.handle >= _sample_port_expressions.size()) {
    details::error("sample port does not belong to this builder");
  }
  auto const& expression = _sample_port_expressions[ref.handle];
  if (expression.channel_type != ref.channel_type)
    details::error("sample port has an invalid semantic channel type");
  return expression.channels;
}

EventPortRef GraphBuilderState::make_event_port(
    EventTypeId type, std::span<EventOutputPortId const> sources)
{
  if (sources.empty())
    details::error("event expression has no semantic sources");
  for (auto source : sources) {
    auto config = _node_bundles.resolve_event_output(
        {source.bundle, PortKind::event, source.port}).config;
    if (config.type != type)
      details::error(
          "event expression source does not match its semantic event type");
  }
  auto const handle = _event_port_expressions.size();
  _event_port_expressions.push_back({
      .type = type,
      .sources = {sources.begin(), sources.end()},
  });
  EventPortRef result;
  result.graph_builder = &facade();
  result.type = type;
  result.handle = handle;
  return result;
}

std::span<EventOutputPortId const>
GraphBuilderState::event_port_sources(EventPortRef const& ref) const
{
  if (ref.graph_builder != &facade()
      || ref.handle >= _event_port_expressions.size()) {
    details::error("event port does not belong to this builder");
  }
  auto const& expression = _event_port_expressions[ref.handle];
  if (expression.type != ref.type)
    details::error("event port has an invalid semantic event type");
  return expression.sources;
}

PublicSampleInputRef GraphBuilderState::input() {
  return input(Sample{0.0f});
}

PublicSampleInputRef GraphBuilderState::input(
    Sample value, std::optional<Sample> min, std::optional<Sample> max) {
  return PublicSampleInputRef(
      _public_ports.add_sample_input(
          *this, _node_bundles, {}, value, min, max));
}

PublicEventInputRef GraphBuilderState::event_input_named(
    std::string_view name, EventTypeId type) {
  return PublicEventInputRef(
      _public_ports.add_event_input(*this, _node_bundles, name, type));
}

PublicEventInputRef GraphBuilderState::event_input(EventTypeId type) {
  return PublicEventInputRef(
      _public_ports.add_event_input(*this, _node_bundles, {}, type));
}

details::SubgraphBuildScope* GraphBuilderState::begin_subgraph() {
  auto const boundary = _node_bundles.append_scope_boundary();
  return new details::SubgraphBuildScope(
      *this, boundary, _node_bundles.size());
}

details::SubgraphBuildScope& GraphBuilderState::require_subgraph_scope(
    details::SubgraphBuildScope& scope) {
  if (scope.owner != this || scope.finished)
    details::error("subgraph build scope is no longer active");
  return scope;
}

void GraphBuilderState::abandon_subgraph(
    details::SubgraphBuildScope* scope) noexcept {
  delete scope;
}

PublicSampleInputRef GraphBuilderState::subgraph_input(
    details::SubgraphBuildScope& scope, std::string_view name,
    ChannelLayout layout, Sample value, std::optional<Sample> min,
    std::optional<Sample> max) {
  auto& active = require_subgraph_scope(scope);
  return PublicSampleInputRef(active.ports.add_sample_input(
      *this, _node_bundles, name, layout, value, min, max));
}

PublicEventInputRef GraphBuilderState::subgraph_event_input(
    details::SubgraphBuildScope& scope, std::string_view name,
    EventTypeId type) {
  auto& active = require_subgraph_scope(scope);
  return PublicEventInputRef(
      active.ports.add_event_input(*this, _node_bundles, name, type));
}

void GraphBuilderState::annotate_public_sample_input_source_info(
    PublicSampleInputRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || ref.port.graph_builder != &facade()) return;
  auto logical = _node_bundles.sample_output_port_for_channels(
      ref.port.channel_type, ref.port.channels());
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

void GraphBuilderState::annotate_public_event_input_source_info(
    PublicEventInputRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || ref.port.graph_builder != &facade()) return;
  auto const sources = ref.port.sources();
  if (sources.size() != 1)
    details::error("PublicEventInputRef has no unique logical boundary port");
  auto source = sources.front();
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

void GraphBuilderState::annotate_sample_port_source_info(
    SamplePortRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || ref.graph_builder != &facade()) return;
  SourceInfo info{
      .declaration_identity = std::string(id),
      .span = {.file_path = std::string(file), .begin = begin, .end = end}};
  _virtual_nodes.attach_sample_output(
      _node_bundles, ref.channel_type, ref.channels(), id, info);
}

void GraphBuilderState::annotate_event_port_source_info(
    EventPortRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || ref.graph_builder != &facade()) return;
  SourceInfo info{
      .declaration_identity = std::string(id),
      .span = {.file_path = std::string(file), .begin = begin, .end = end}};
  _virtual_nodes.attach_event_output(
      _node_bundles, ref.type, ref.sources(), id, info);
}

void GraphBuilderState::annotate_public_sample_output_source_info(
    std::span<SourceInfo const> infos) {
  for (size_t i = 0; i < infos.size(); ++i)
    _public_ports.annotate_sample_output_source_info(i, infos[i]);
}

void GraphBuilderState::annotate_public_event_output_source_info(
    std::span<SourceInfo const> infos) {
  for (size_t i = 0; i < infos.size(); ++i)
    _public_ports.annotate_event_output_source_info(i, infos[i]);
}

void GraphBuilderState::annotate_public_sample_output_source_info(
    size_t ordinal, SourceInfo info) {
  _public_ports.annotate_sample_output_source_info(
      ordinal, std::move(info));
}

void GraphBuilderState::annotate_public_event_output_source_info(
    size_t ordinal, SourceInfo info) {
  _public_ports.annotate_event_output_source_info(
      ordinal, std::move(info));
}

NodeRef GraphBuilderState::embed_subgraph(
    GraphBuilderState const& child, std::string_view kind) {
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
      facade(), _node_bundles.append_subgraph(boundary, begin, count, kind));
}

void GraphBuilderState::event_outputs(
    std::span<EventOutputRequest const> refs) {
  auto configs = make_event_output_configs(refs);
  _public_ports.define_event_outputs(
      *this, _node_bundles, _identity, configs);
}

void GraphBuilderState::subgraph_event_outputs(
    details::SubgraphBuildScope& scope,
    std::span<EventOutputRequest const> refs) {
  auto& active = require_subgraph_scope(scope);
  auto configs = make_event_output_configs(refs);
  active.ports.define_event_outputs(*this, _node_bundles, _identity, configs);
}

void GraphBuilderState::outputs(
    std::span<SampleOutputRequest const> refs) {
  auto configs = make_sample_output_configs(refs);
  _public_ports.define_sample_outputs(
      *this, _node_bundles, _identity, configs);
}

void GraphBuilderState::subgraph_outputs(
    details::SubgraphBuildScope& scope,
    std::span<SampleOutputRequest const> refs) {
  auto& active = require_subgraph_scope(scope);
  auto configs = make_sample_output_configs(refs);
  active.ports.define_sample_outputs(*this, _node_bundles, _identity, configs);
}

void GraphBuilderState::subgraph_outputs(
    details::SubgraphBuildScope& scope, std::span<NamedRef const> refs) {
  auto& active = require_subgraph_scope(scope);
  active.ports.define_sample_outputs_from_named_refs(
      *this, _node_bundles, _identity,
      [&](auto&& value) {
        return lift_to_sample_port(std::forward<decltype(value)>(value));
      },
      refs);
}

NodeRef GraphBuilderState::finish_subgraph(
    details::SubgraphBuildScope& scope, std::string_view kind) {
  auto& active = require_subgraph_scope(scope);
  auto const child_count = _node_bundles.size() - active.child_begin;
  NodeRef result(facade(), _node_bundles.append_subgraph(
      active.ports.boundary_handle(), active.child_begin, child_count, kind));
  for (auto const& info :
       _node_bundles.bundle(active.ports.boundary_handle())
           .source_annotations().infos) {
    result._annotate_source_info(
        info.declaration_identity,
        info.span.file_path, info.span.begin, info.span.end);
  }
  active.finished = true;
  return result;
}

GraphBuilderState::VacantInputs GraphBuilderState::vacant_inputs() const {
  return _connections.collect_vacant_inputs(_node_bundles, _virtual_nodes);
}

GraphBuilderState::VirtualInputs GraphBuilderState::virtual_inputs() const {
  return _connections.collect_virtual_inputs(_node_bundles, _virtual_nodes);
}

GraphBuilderState::VirtualSampleInputFamilies
GraphBuilderState::virtual_sample_input_families() const {
  return _connections.collect_virtual_sample_input_families(
      _node_bundles, _virtual_nodes);
}

GraphBuilderState::VirtualOutputs GraphBuilderState::virtual_outputs() const {
  return _connections.collect_virtual_outputs(_node_bundles, _virtual_nodes);
}

GraphBuilderState::VirtualSampleOutputFamilies
GraphBuilderState::virtual_sample_output_families() const {
  return _connections.collect_virtual_sample_output_families(
      _node_bundles, _virtual_nodes);
}

GraphBuilderState::VirtualPorts GraphBuilderState::virtual_ports() const {
  return _virtual_nodes.ports(_node_bundles);
}

void GraphBuilderState::record_authored_sample_connection(
    NodeBundlePortId target, std::span<SamplePortRef const> sources) {
  auto targets = _node_bundles.sample_input_channels(target);
  auto type = _node_bundles.resolve_sample_input(target)
                  .config.channel_layout.channel_type;
  if (sources.size() != targets.size())
    details::error(
        "sample channel source count does not match NodeBundle port layout");
  std::vector<SampleOutputChannelId> channels;
  for (auto const& source : sources) {
    if (source.graph_builder != &facade() ||
        source.channel_type != ChannelTypeId::mono ||
        source.handle >= _sample_port_expressions.size())
      details::error(
          "channel-wise sample connection requires scalar sources");
    auto const source_channels = source.channels();
    if (source_channels.size() != 1)
      details::error(
          "channel-wise sample connection requires scalar sources");
    channels.push_back(source_channels.front());
  }
  _connections.record_authored_sample_connection(
      {type, std::move(channels), type, std::move(targets)});
}

void GraphBuilderState::record_authored_event_connection(
    NodeBundlePortId target, EventPortRef const& source) {
  if (target.port_kind != PortKind::event ||
      source.graph_builder != &facade() || source.handle >= _event_port_expressions.size())
    details::error("invalid authored event connection");
  auto const sources = source.sources();
  if (sources.empty())
    details::error("invalid authored event connection");
  auto target_type = _node_bundles.resolve_event_input(target).config.type;
  _connections.record_authored_event_connection({
      source.type, {sources.begin(), sources.end()}, target_type,
      _node_bundles.event_input_ports(target)});
}

void GraphBuilderState::connect_sample_input(
    NodeBundlePortId target, std::span<SamplePortRef const> sources) {
  record_authored_sample_connection(target, sources);
}

void GraphBuilderState::connect_event_input(
    NodeBundlePortId target, EventPortRef source) {
  record_authored_event_connection(target, source);
}

bool GraphBuilderState::sample_input_is_connected(
    NodeBundlePortId target) const {
  if (target.port_kind != PortKind::sample)
    details::error("sample connectivity requested for event port");
  auto channels = _node_bundles.sample_input_channels(target);
  return std::ranges::any_of(channels, [&](auto channel) {
    return _connections.sample_input_is_connected(channel);
  });
}

bool GraphBuilderState::event_input_is_connected(
    NodeBundlePortId target) const {
  if (target.port_kind != PortKind::event)
    details::error("event connectivity requested for sample port");
  auto ports = _node_bundles.event_input_ports(target);
  return std::ranges::any_of(ports, [&](auto port) {
    return _connections.event_input_is_connected(port);
  });
}

void GraphBuilderState::connect_sample_output(
    NodeBundlePortId source, NodeRef const& target) {
  auto semantic = SamplePortRef(facade(), source);
  auto const& sink = _node_bundles.bundle(target.node_bundle_handle());
  if (!sink.is_concrete())
    details::error("a graph-service sink must be one concrete node bundle");
  if (semantic.channels().size() != sink.sample_input_count())
    details::error(
        "NodeBundle output does not match graph-service sink channel count");
  for (size_t i = 0; i < sink.sample_input_count(); ++i)
    record_authored_sample_connection(
        {target.node_bundle_handle(), PortKind::sample, i},
        semantic.select_channel(i));
}

size_t GraphBuilderState::sample_input_count(NodeBundleHandle handle) const {
  return _node_bundles.bundle(handle).sample_input_count();
}

size_t GraphBuilderState::sample_output_count(NodeBundleHandle handle) const {
  return _node_bundles.bundle(handle).sample_output_count();
}

size_t GraphBuilderState::event_input_count(NodeBundleHandle handle) const {
  return _node_bundles.bundle(handle).event_input_count();
}

size_t GraphBuilderState::event_output_count(NodeBundleHandle handle) const {
  return _node_bundles.bundle(handle).event_output_count();
}

NodeBundleHandle GraphBuilderState::tiled_member(
    NodeBundleHandle handle, size_t channel) const {
  return _node_bundles.tiled_member(handle, channel);
}

NodePorts const& GraphBuilderState::typed_ports(NodeBundleHandle handle) const {
  return _node_bundles.typed_ports(handle);
}

SamplePortRef GraphBuilderState::sample_port_from_output(NodeBundlePortId port) {
  if (port.port_kind != PortKind::sample)
    details::error("attempted to read a sample port from an event output");
  auto descriptor = _node_bundles.resolve_sample_output(port);
  return make_sample_port(
      descriptor.config.channel_layout.channel_type,
      _node_bundles.sample_output_channels(port));
}

EventPortRef GraphBuilderState::event_port_from_output(
    NodeBundlePortId port) const {
  if (port.port_kind != PortKind::event)
    details::error("attempted to read an event port from a sample output");
  auto descriptor = _node_bundles.resolve_event_output(port);
  return const_cast<GraphBuilderState&>(*this).make_event_port(
      descriptor.config.type, _node_bundles.event_output_ports(port));
}

void GraphBuilderState::apply_ttl(NodeBundleHandle handle, size_t samples) {
  _node_bundles.apply_ttl(handle, samples);
}

void GraphBuilderState::annotate_node(
    NodeBundleHandle handle, std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) {
  _annotations.annotate_node_source_info(
      _node_bundles, _virtual_nodes, _identity, handle, id, file, begin, end);
}

EventPortRef GraphBuilderState::event_output(
    NodeBundlePortId source) const {
  if (source.port_kind != PortKind::event)
    details::error(
        "attempted to read an event output from a sample NodeBundle port");
  return EventPortRef(facade(), source);
}

size_t GraphBuilderState::sample_port_index(
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

size_t GraphBuilderState::event_port_index(
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
