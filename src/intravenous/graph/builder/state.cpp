#include <intravenous/graph/builder/state.h>

#include <intravenous/graph/builder/embedder.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

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
    if (ref.name.starts_with("__")) {
      details::error("generated channel assignments are not public outputs");
    }
    configs.push_back({
        .ref = ref.ref,
        .config = {
            .name = std::string(ref.name),
            .channel_layout = ref.channel_layout,
            .production = default_tick_output_properties(
                ContinuousValueTypeSet::audio_capable()),
        },
        .public_member = {
            .family_name = std::string(ref.family_name),
            .channel_type = ref.family_channel_type,
            .whole_stream = ref.whole_stream,
        },
        .target_channel_index = ref.targets_single_channel()
            ? std::optional{ref.target_channel_index}
            : std::nullopt,
    });
  }
  return configs;
}

SourceInfo make_source_info(
    std::string_view id, std::string_view file, uint32_t begin, uint32_t end) {
  return SourceInfo{
      .declaration_identity = std::string(id),
      .span = {.file_path = std::string(file), .begin = begin, .end = end},
  };
}

// Every node-bundle reference held by a virtual-node record.
template<class Visit>
void for_each_record_bundle(VirtualNodeRecord& record, Visit&& visit) {
  for (auto& bundle : record.node_bundle_handles) visit(bundle);
  for (auto& mapping : record.sample_inputs) {
    for (auto& channel : mapping.channels) visit(channel.bundle);
    for (auto& members : mapping.member_channels)
      for (auto& channel : members) visit(channel.bundle);
  }
  for (auto& mapping : record.sample_outputs) {
    for (auto& channel : mapping.channels) visit(channel.bundle);
    for (auto& members : mapping.member_channels)
      for (auto& channel : members) visit(channel.bundle);
  }
  for (auto& mapping : record.event_inputs)
    for (auto& port : mapping.node_bundle_ports) visit(port.node_bundle_handle);
  for (auto& mapping : record.event_outputs)
    for (auto& port : mapping.node_bundle_ports) visit(port.node_bundle_handle);
}

template<class Predicate>
bool record_bundles_all(VirtualNodeRecord const& record, Predicate&& predicate) {
  bool result = true;
  for_each_record_bundle(const_cast<VirtualNodeRecord&>(record),
      [&](NodeBundleHandle& bundle) { result = result && predicate(bundle); });
  return result;
}

template<class Predicate>
bool record_bundles_any(VirtualNodeRecord const& record, Predicate&& predicate) {
  bool result = false;
  for_each_record_bundle(const_cast<VirtualNodeRecord&>(record),
      [&](NodeBundleHandle& bundle) { result = result || predicate(bundle); });
  return result;
}

template<class Predicate>
bool sample_connection_all(
    ConfiguredSampleConnection const& connection, Predicate&& predicate) {
  return std::ranges::all_of(connection.source_channels,
             [&](auto const& source) { return predicate(source.channel.bundle); })
      && std::ranges::all_of(connection.target_channels,
             [&](auto const& target) { return predicate(target.bundle); });
}

template<class Predicate>
bool event_connection_all(
    ConfiguredEventConnection const& connection, Predicate&& predicate) {
  return std::ranges::all_of(connection.sources,
             [&](auto const& source) { return predicate(source.port.bundle); })
      && std::ranges::all_of(connection.targets,
             [&](auto const& target) { return predicate(target.bundle); });
}

template<class Map>
ConfiguredSampleConnection remap_sample_connection(
    ConfiguredSampleConnection connection, Map&& map) {
  for (auto& source : connection.source_channels)
    source.channel.bundle = map(source.channel.bundle);
  for (auto& target : connection.target_channels) target.bundle = map(target.bundle);
  return connection;
}

template<class Map>
ConfiguredEventConnection remap_event_connection(
    ConfiguredEventConnection connection, Map&& map) {
  for (auto& source : connection.sources) source.port.bundle = map(source.port.bundle);
  for (auto& target : connection.targets) target.bundle = map(target.bundle);
  return connection;
}

constexpr std::uint64_t imported_virtual_namespace =
    std::numeric_limits<std::uint64_t>::max();
} // namespace

// ---- GraphBuilderPublicPorts members that create session-owned refs ------

SamplePortRef GraphBuilderPublicPorts::add_continuous_input(
    GraphBuilderState& builder, GraphBuilderNodeBundles& bundles,
    ContinuousInputConfig config) {
  if (!is_sequential(config)) {
    details::error(
        "graph and subgraph boundary inputs must use Sequential access");
  }
  if (continuous_value_type_domain(config).empty()) {
    details::error(
        "graph and subgraph boundary inputs must declare a continuous value type");
  }
  auto index = bundles.bundle(_boundary).append_boundary_sample_input(
      std::move(config));
  _sample_input_source_infos.emplace_back();
  return builder.sample_port_from_output(
      NodeBundlePortId{_boundary, PortKind::sample, index});
}

EventPortRef GraphBuilderPublicPorts::add_event_input(
    GraphBuilderState& builder, GraphBuilderNodeBundles& bundles,
    std::string_view name, EventTypeId type) {
  auto index = bundles.bundle(_boundary).append_boundary_event_input(
      {.name = std::string(name), .type = type});
  _event_input_source_infos.emplace_back();
  return builder.event_port_from_output(
      NodeBundlePortId{_boundary, PortKind::event, index});
}

void GraphBuilderPublicPorts::define_sample_outputs(
    GraphBuilderState& builder, GraphBuilderNodeBundles& bundles,
    GraphBuilderIdentity const& identity, std::span<OutputRefConfig const> refs) {
  _last_sample_output_port_indices.clear();
  bool require_names = refs.size() > 1;
  for (size_t i = 0; i < refs.size(); ++i) {
    auto const& ref = refs[i].ref; auto const& config = refs[i].config;
    if (!ref.session) details::error("builder " + identity.value + ": outputs(...): empty SamplePortRef");
    if (ref.session != builder.session()) details::error("builder " + identity.value + ": outputs(...): SamplePortRef belongs to another builder session");
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
    if (refs[i].target_channel_index) {
      auto channels = bundles.sample_input_channels(target); auto channel = *refs[i].target_channel_index;
      if (channel >= channels.size()) details::error("public sample output channel index is out of bounds");
      builder.record_configured_sample_connection(channels[channel], ref);
    } else builder.record_configured_sample_connection(target, ref);
    _last_sample_output_port_indices.push_back(output);
  }
  _sample_outputs_defined = true;
}

void GraphBuilderPublicPorts::define_event_outputs(
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
    if (!ref.session)
      details::error(
          "builder " + identity.value +
          ": event_outputs(...): empty EventPortRef");
    if (ref.session != builder.session())
      details::error(
          "builder " + identity.value +
          ": event_outputs(...): EventPortRef belongs to another builder session");
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

// ---- construction --------------------------------------------------------

GraphBuilderState::GraphBuilderState(
    details::BuilderSession* session, GraphBuilderIdentity identity)
    : _session(session)
    , _identity(std::move(identity))
{
  auto const boundary = _node_bundles.append_boundary();
  _scopes.push_back(ScopeFrame{
      .id = details::root_builder_scope_id,
      .kind = details::BuilderScopeKind::root,
      .ports = GraphBuilderPublicPorts(boundary),
      .virtual_namespace = 0,
      .namespace_begin = 0,
      .record_begin = 0,
  });
}

GraphBuilderMark GraphBuilderState::mark() const noexcept
{
  return GraphBuilderMark{
      .bundles = _node_bundles.size(),
      .sample_connections = _connections.sample_connection_count(),
      .event_connections = _connections.event_connection_count(),
      .virtual_records = _virtual_nodes.size(),
  };
}

// ---- scopes --------------------------------------------------------------

std::size_t GraphBuilderState::frame_index(details::BuilderScopeId id) const
{
  for (auto index = _scopes.size(); index-- > 0;) {
    if (_scopes[index].id == id) return index;
  }
  details::error("builder scope is no longer open");
}

GraphBuilderState::ScopeFrame& GraphBuilderState::frame(details::BuilderScopeId id)
{
  return _scopes[frame_index(id)];
}

GraphBuilderState::ScopeFrame const& GraphBuilderState::frame(
    details::BuilderScopeId id) const
{
  return _scopes[frame_index(id)];
}

bool GraphBuilderState::scope_is_open(details::BuilderScopeId id) const noexcept
{
  return std::ranges::any_of(
      _scopes, [&](ScopeFrame const& scope) { return scope.id == id; });
}

details::BuilderScopeId GraphBuilderState::current_scope_id() const noexcept
{
  return _scopes.back().id;
}

std::uint64_t GraphBuilderState::current_namespace() const noexcept
{
  return _scopes.back().virtual_namespace;
}

bool GraphBuilderState::scope_sample_outputs_defined(details::BuilderScopeId id) const
{
  return frame(id).ports.sample_outputs_defined();
}

NodeBundleHandle GraphBuilderState::scope_boundary(details::BuilderScopeId id) const
{
  return frame(id).ports.boundary_handle();
}

details::BuilderScopeId GraphBuilderState::push_scope(details::BuilderScopeKind kind)
{
  if (kind == details::BuilderScopeKind::root) {
    details::error("a builder session has exactly one root scope");
  }
  auto const& parent = _scopes.back();
  auto const parent_namespace = parent.virtual_namespace;
  auto const parent_namespace_begin = parent.namespace_begin;
  auto const boundary = _node_bundles.append_scope_boundary();
  auto const definition = kind == details::BuilderScopeKind::definition;
  _scopes.push_back(ScopeFrame{
      .id = _next_scope_id++,
      .kind = kind,
      .ports = GraphBuilderPublicPorts(boundary),
      .virtual_namespace = definition ? _next_namespace++ : parent_namespace,
      .namespace_begin = definition ? boundary : parent_namespace_begin,
      .record_begin = _virtual_nodes.size(),
  });
  return _scopes.back().id;
}

std::string GraphBuilderState::relative_scope_qualifier(NodeBundleHandle subgraph) const
{
  return "@scope:" + std::to_string(subgraph - _scopes.back().namespace_begin);
}

NodeBundleHandle GraphBuilderState::finish_scope(
    details::BuilderScopeId id, std::string_view kind,
    std::optional<std::string_view> qualifier)
{
  auto const index = frame_index(id);
  if (index + 1 != _scopes.size()) {
    details::error("a nested builder scope must be finished before its parent");
  }
  auto& scope = _scopes.back();
  if (scope.kind == details::BuilderScopeKind::root) {
    details::error("the root builder scope is finished by GraphBuilder::finish()");
  }
  auto const boundary = scope.ports.boundary_handle();

  if (scope.kind == details::BuilderScopeKind::subgraph) {
    auto const child_begin = boundary + 1;
    _scopes.pop_back();
    return _node_bundles.append_subgraph(
        boundary, child_begin, _node_bundles.size() - child_begin, kind);
  }

  if (!scope.ports.sample_outputs_defined()) {
    details::error("embedded graph must call g.outputs(...) before insertion");
  }
  auto const subgraph = _node_bundles.append_subgraph(
      boundary, boundary, _node_bundles.size() - boundary, kind);
  // Public input declarations become input ports of the subgraph. They are
  // recorded inside this definition's namespace and qualified with it, as
  // they are when the same configured graph is imported frozen.
  auto const sample_inputs = scope.ports.sample_inputs(_node_bundles);
  for (size_t input = 0; input < sample_inputs.size(); ++input) {
    for (auto const& info : scope.ports.sample_input_source_infos(input)) {
      attach_virtual_input_source_info(
          subgraph, info.declaration_identity, PortKind::sample,
          sample_inputs[input].name, info);
    }
  }
  auto const event_inputs = scope.ports.event_inputs(_node_bundles);
  for (size_t input = 0; input < event_inputs.size(); ++input) {
    for (auto const& info : scope.ports.event_input_source_infos(input)) {
      attach_virtual_input_source_info(
          subgraph, info.declaration_identity, PortKind::event,
          event_inputs[input].name, info);
    }
  }
  auto const record_begin = scope.record_begin;
  _scopes.pop_back();
  if (qualifier) {
    _virtual_nodes.qualify(record_begin, *qualifier);
  } else {
    _virtual_nodes.qualify(record_begin, relative_scope_qualifier(subgraph));
  }
  return subgraph;
}

std::vector<SourceInfo> GraphBuilderState::subgraph_boundary_source_infos(
    NodeBundleHandle subgraph) const
{
  auto const boundary = _node_bundles.bundle(subgraph).subgraph_boundary_handle();
  if (!boundary) details::error("node bundle is not a subgraph");
  return _node_bundles.bundle(*boundary).source_annotations().infos;
}

PublicSampleInputRef GraphBuilderState::scope_input(
    details::BuilderScopeId id, ContinuousInputConfig config)
{
  auto const index = frame_index(id);
  return PublicSampleInputRef(_scopes[index].ports.add_continuous_input(
      *this, _node_bundles, std::move(config)));
}

PublicEventInputRef GraphBuilderState::scope_event_input(
    details::BuilderScopeId id, std::string_view name, EventTypeId type)
{
  auto const index = frame_index(id);
  return PublicEventInputRef(_scopes[index].ports.add_event_input(
      *this, _node_bundles, name, type));
}

void GraphBuilderState::scope_outputs(
    details::BuilderScopeId id, std::span<SampleOutputRequest const> refs)
{
  auto const index = frame_index(id);
  auto configs = make_sample_output_configs(refs);
  _scopes[index].ports.define_sample_outputs(
      *this, _node_bundles, _identity, configs);
}

void GraphBuilderState::scope_outputs(
    details::BuilderScopeId id, std::span<NamedRef const> refs)
{
  auto const index = frame_index(id);
  _scopes[index].ports.define_sample_outputs_from_named_refs(
      *this, _node_bundles, _identity,
      [&](NamedRef const& value) { return lift_named_sample(value); },
      refs);
}

void GraphBuilderState::scope_event_outputs(
    details::BuilderScopeId id, std::span<EventOutputRequest const> refs)
{
  auto const index = frame_index(id);
  auto configs = make_event_output_configs(refs);
  _scopes[index].ports.define_event_outputs(
      *this, _node_bundles, _identity, configs);
}

void GraphBuilderState::annotate_scope_sample_output_source_info(
    details::BuilderScopeId id, std::span<SourceInfo const> infos)
{
  auto const index = frame_index(id);
  for (size_t i = 0; i < infos.size(); ++i)
    _scopes[index].ports.annotate_sample_output_source_info(i, infos[i]);
}

void GraphBuilderState::annotate_scope_event_output_source_info(
    details::BuilderScopeId id, std::span<SourceInfo const> infos)
{
  auto const index = frame_index(id);
  for (size_t i = 0; i < infos.size(); ++i)
    _scopes[index].ports.annotate_event_output_source_info(i, infos[i]);
}

void GraphBuilderState::annotate_scope_sample_output_source_info(
    details::BuilderScopeId id, std::size_t output, SourceInfo info)
{
  auto const index = frame_index(id);
  _scopes[index].ports.annotate_sample_output_source_info(output, std::move(info));
}

void GraphBuilderState::annotate_scope_event_output_source_info(
    details::BuilderScopeId id, std::size_t output, SourceInfo info)
{
  auto const index = frame_index(id);
  _scopes[index].ports.annotate_event_output_source_info(output, std::move(info));
}

SamplePortRef GraphBuilderState::lift_named_sample(NamedRef const& ref)
{
  return std::visit(
      [&](auto const& value) -> SamplePortRef {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<T, EventPortRef>) {
          details::error("expected sample value, got event");
        } else if constexpr (std::same_as<T, Sample>) {
          GraphBuilder builder(_session);
          return builder.lift_to_sample_port(value);
        } else {
          return require_sample_port(value);
        }
      },
      ref.value);
}

// ---- node construction -------------------------------------------------

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
  for (auto const& config : concrete.ports.sample_inputs()) {
    if (config.channel_layout.channel_type != ChannelTypeId::mono)
      details::error(
          "the tiled-node model requires fully mono concrete sample nodes");
  }
  for (auto const& config : concrete.ports.sample_outputs()) {
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

NodeBundleHandle GraphBuilderState::append_tiled_node_bundles(
    std::span<NodeBundleHandle const> members, ChannelLayout layout)
{
  return _node_bundles.append_tiled(members, layout);
}

void GraphBuilderState::set_registered_node_type_identity(
    NodeBundleHandle handle, RegisteredNodeTypeIdentity identity)
{
  _node_bundles.set_registered_node_type_identity(handle, std::move(identity));
}

void GraphBuilderState::validate_tiled_module_interfaces(
    std::span<NodeBundleHandle const> members) const
{
  if (members.empty()) {
    details::error("tiled IV module requires child graphs");
  }
  auto boundary_of = [&](NodeBundleHandle member) -> NodeBundle const& {
    auto const boundary = _node_bundles.bundle(member).subgraph_boundary_handle();
    if (!boundary) details::error("tiled IV module member is not a module scope");
    return _node_bundles.bundle(*boundary);
  };
  auto const& first = boundary_of(members.front());
  auto const first_inputs = first.boundary_sample_inputs();
  auto const first_outputs = first.boundary_sample_outputs();
  auto const first_event_inputs = first.boundary_event_inputs();
  auto const first_event_outputs = first.boundary_event_outputs();

  for (auto const member : members) {
    auto const& boundary = boundary_of(member);
    auto const inputs = boundary.boundary_sample_inputs();
    auto const outputs = boundary.boundary_sample_outputs();
    auto const event_inputs = boundary.boundary_event_inputs();
    auto const event_outputs = boundary.boundary_event_outputs();
    for (auto const& config : inputs) {
      if (config.channel_layout.channel_type != ChannelTypeId::mono) {
        details::error(
            "tiled IV module members must expose only mono sample inputs");
      }
    }
    for (auto const& config : outputs) {
      if (config.channel_layout.channel_type != ChannelTypeId::mono) {
        details::error(
            "tiled IV module members must expose only mono sample outputs");
      }
    }
    if (!std::ranges::equal(first_inputs, inputs)
        || !std::ranges::equal(first_outputs, outputs)
        || !std::ranges::equal(first_event_inputs, event_inputs)
        || !std::ranges::equal(first_event_outputs, event_outputs)) {
      details::error(
          "tiled IV module members do not expose equivalent port configurations");
    }
  }
}

// ---- virtual records ----------------------------------------------------

std::string GraphBuilderState::virtual_record_key(
    std::uint64_t name_space, std::string_view source_identity,
    std::string_view type_identity)
{
  std::string key = std::to_string(name_space);
  key += '\x1f';
  key += source_identity;
  key += '\x1f';
  key += type_identity;
  return key;
}

VirtualNodeHandle GraphBuilderState::append_virtual_record(
    std::string_view source_identity, std::string_view type_identity,
    std::uint64_t name_space)
{
  auto const handle = _virtual_nodes.create(source_identity, type_identity);
  _virtual_namespaces.push_back(name_space);
  return handle;
}

VirtualNodeHandle GraphBuilderState::virtual_record(
    std::string_view source_identity, std::string_view type_identity)
{
  auto const name_space = current_namespace();
  auto key = virtual_record_key(name_space, source_identity, type_identity);
  if (auto const found = _virtual_record_index.find(key);
      found != _virtual_record_index.end()) {
    return found->second;
  }
  auto const handle = append_virtual_record(
      source_identity, type_identity, name_space);
  _virtual_record_index.emplace(std::move(key), handle);
  return handle;
}

void GraphBuilderState::index_imported_records(std::size_t begin)
{
  _virtual_namespaces.resize(begin);
  _virtual_namespaces.resize(_virtual_nodes.size(), imported_virtual_namespace);
}

void GraphBuilderState::qualify_virtual_records(
    std::size_t begin, std::string_view qualifier)
{
  _virtual_nodes.qualify(begin, qualifier);
}

void GraphBuilderState::attach_virtual_bundle_member(
    NodeBundleHandle bundle, std::string_view source_identity,
    SourceInfo const* source_info)
{
  if (source_identity.empty()) return;
  auto const type_identity = std::string(_node_bundles.bundle(bundle).type_identity());
  auto const record = virtual_record(source_identity, type_identity);
  _virtual_nodes.attach_bundle_member(_node_bundles, record, bundle, source_info);
}

void GraphBuilderState::attach_virtual_input_source_info(
    NodeBundleHandle bundle, std::string_view source_identity, PortKind kind,
    std::string_view port_name, SourceInfo const& info)
{
  if (source_identity.empty()) return;
  auto const type_identity = std::string(_node_bundles.bundle(bundle).type_identity());
  auto const record = virtual_record(source_identity, type_identity);
  _virtual_nodes.annotate_input_source_info(
      _node_bundles, record, bundle, kind, port_name, info);
}

// ---- frozen graphs -----------------------------------------------------

ConfiguredGraphEmbedding GraphBuilderState::embed_configured_graph(
    ConfiguredGraph const& child, std::string_view kind)
{
  if (!child.public_ports.sample_outputs_defined())
    details::error(
        "configured graph " + child.identity.value
        + ": g.outputs(...) must be called before insertion");

  auto const begin = _node_bundles.size();
  auto const record_begin = _virtual_nodes.size();
  auto const root_scope = begin + child.node_bundles.size();
  auto imported = GraphBuilderChildEmbedder::import(
      _node_bundles, _connections, _virtual_nodes,
      child.node_bundles, child.connections, child.virtual_nodes,
      relative_scope_qualifier(root_scope));
  index_imported_records(record_begin);
  IV_ASSERT(imported.bundle_offset == begin,
      "embedded child bundle offset changed unexpectedly");

  auto const boundary = imported.bundle_offset
      + child.public_ports.boundary_handle();
  auto const subgraph = _node_bundles.append_subgraph(
      boundary, begin, child.node_bundles.size(), kind);
  IV_ASSERT(subgraph == root_scope, "embedded root scope handle changed unexpectedly");

  // Once a child graph is embedded, its public inputs become input ports of
  // the surrounding subgraph. Keep child declaration/reference source
  // identities on those virtual ports so source introspection does not lose
  // provenance at the graph boundary.
  auto const sample_inputs = child.public_ports.sample_inputs(child.node_bundles);
  for (size_t index = 0; index < sample_inputs.size(); ++index) {
    for (auto const& info : child.public_ports.sample_input_source_infos(index)) {
      attach_virtual_input_source_info(
          subgraph, info.declaration_identity, PortKind::sample,
          sample_inputs[index].name, info);
    }
  }
  auto const event_inputs = child.public_ports.event_inputs(child.node_bundles);
  for (size_t index = 0; index < event_inputs.size(); ++index) {
    for (auto const& info : child.public_ports.event_input_source_infos(index)) {
      attach_virtual_input_source_info(
          subgraph, info.declaration_identity, PortKind::event,
          event_inputs[index].name, info);
    }
  }

  return ConfiguredGraphEmbedding{
      .root_scope = subgraph,
      .node_bundles = std::move(imported.node_bundles),
      .virtual_nodes = std::move(imported.virtual_nodes),
  };
}

std::optional<GraphBuilderInvocationSnapshot> GraphBuilderState::snapshot_contents(
    GraphBuilderMark const& begin, NodeBundleHandle result) const
{
  auto const first = begin.bundles;
  auto const end = _node_bundles.size();
  auto const record_begin = begin.virtual_records;
  auto const inside = [&](NodeBundleHandle handle) {
    return handle >= first && handle < end;
  };
  // Closedness is structural. A cacheable invocation cannot change older
  // graph state: its definition scope owns a fresh virtual-record namespace,
  // and the only way to reach an older bundle is through a NodeRef or
  // SubgraphClosure argument, which already makes the invocation
  // uncacheable. Older records therefore need no scan here.
  if (!inside(result)) return std::nullopt;

  auto const samples = _connections.configured_sample_connections();
  auto const events = _connections.configured_event_connections();
  for (auto index = begin.sample_connections; index < samples.size(); ++index) {
    if (!sample_connection_all(samples[index], inside)) return std::nullopt;
  }
  for (auto index = begin.event_connections; index < events.size(); ++index) {
    if (!event_connection_all(events[index], inside)) return std::nullopt;
  }
  auto const& records = _virtual_nodes.records();
  for (auto index = record_begin; index < records.size(); ++index) {
    if (!record_bundles_all(records[index], inside)) return std::nullopt;
  }

  auto const local = [&](NodeBundleHandle handle) { return handle - first + 1; };
  std::vector<ConfiguredSampleConnection> local_samples;
  for (auto index = begin.sample_connections; index < samples.size(); ++index) {
    local_samples.push_back(remap_sample_connection(samples[index], local));
  }
  std::vector<ConfiguredEventConnection> local_events;
  for (auto index = begin.event_connections; index < events.size(); ++index) {
    local_events.push_back(remap_event_connection(events[index], local));
  }
  std::vector<VirtualNodeRecord> local_records(
      records.begin() + static_cast<std::ptrdiff_t>(record_begin), records.end());
  for (auto& record : local_records) {
    for_each_record_bundle(record, [&](NodeBundleHandle& bundle) {
      bundle = local(bundle);
    });
  }
  ConfiguredPublicPortsRecord ports{
      .boundary = 0,
      .sample_outputs_defined = true,
  };
  return GraphBuilderInvocationSnapshot{
      .graph = ConfiguredGraph{
          .identity = _identity,
          .node_bundles = _node_bundles.copy_range(first, end, 1, record_begin),
          .connections = GraphBuilderConnections::from_configured_connections(
              local_samples, local_events),
          .public_ports = GraphBuilderPublicPorts::from_configured_record(ports),
          .annotations = {},
          .virtual_nodes = GraphBuilderVirtualNodes::from_configured_records(
              local_records),
      },
      .local_root = local(result),
  };
}

NodeBundleHandle GraphBuilderState::import_contents(
    ConfiguredGraph const& graph, NodeBundleHandle local_root,
    std::string_view qualifier)
{
  if (local_root == 0 || local_root >= graph.node_bundles.size()) {
    details::error("invocation snapshot has an invalid local root");
  }
  auto const record_begin = _virtual_nodes.size();
  auto const offset = _node_bundles.import_contents(graph.node_bundles);
  _connections.import_child(graph.connections, offset);
  (void)_virtual_nodes.import_child(
      _node_bundles, graph.virtual_nodes, offset, qualifier);
  index_imported_records(record_begin);
  return local_root + offset;
}

std::expected<ConfiguredGraph, ConfiguredGraphSnapshotError>
GraphBuilderState::snapshot_scope(details::BuilderScopeId id) const
{
  using Kind = ConfiguredGraphSnapshotError::Kind;
  auto const found = std::ranges::find(_scopes, id, &ScopeFrame::id);
  if (found == _scopes.end()) {
    return std::unexpected(ConfiguredGraphSnapshotError{
        .kind = Kind::closed_scope,
        .message = "the builder scope is no longer open",
    });
  }
  if (std::next(found) != _scopes.end()) {
    return std::unexpected(ConfiguredGraphSnapshotError{
        .kind = Kind::open_nested_scope,
        .message = "the builder scope still has an open nested scope",
    });
  }
  auto const& scope = *found;
  auto const first = scope.ports.boundary_handle();
  auto const end = _node_bundles.size();
  auto const inside = [&](NodeBundleHandle handle) {
    return handle >= first && handle < end;
  };
  auto const capture = [&](std::string message) {
    return std::unexpected(ConfiguredGraphSnapshotError{
        .kind = Kind::free_graph_capture,
        .message = std::move(message),
    });
  };

  std::vector<ConfiguredSampleConnection> local_samples;
  for (auto const& connection : _connections.configured_sample_connections()) {
    if (!sample_connection_all(connection, [&](auto h) { return !inside(h); })) {
      if (!sample_connection_all(connection, inside)) {
        return capture("a sample connection crosses the scope boundary");
      }
      local_samples.push_back(remap_sample_connection(
          connection, [&](NodeBundleHandle h) { return h - first; }));
    }
  }
  std::vector<ConfiguredEventConnection> local_events;
  for (auto const& connection : _connections.configured_event_connections()) {
    if (!event_connection_all(connection, [&](auto h) { return !inside(h); })) {
      if (!event_connection_all(connection, inside)) {
        return capture("an event connection crosses the scope boundary");
      }
      local_events.push_back(remap_event_connection(
          connection, [&](NodeBundleHandle h) { return h - first; }));
    }
  }
  auto const& records = _virtual_nodes.records();
  for (std::size_t index = 0; index < scope.record_begin; ++index) {
    if (record_bundles_any(records[index], inside)) {
      return std::unexpected(ConfiguredGraphSnapshotError{
          .kind = Kind::enclosing_scope_mutation,
          .message = "virtual node '" + records[index].id
              + "' of an enclosing scope has members inside the scope",
      });
    }
  }
  std::vector<VirtualNodeRecord> local_records;
  for (auto index = scope.record_begin; index < records.size(); ++index) {
    if (!record_bundles_all(records[index], inside)) {
      return capture("virtual node '" + records[index].id
          + "' has members outside the scope");
    }
    auto record = records[index];
    for_each_record_bundle(record, [&](NodeBundleHandle& bundle) {
      bundle -= first;
    });
    local_records.push_back(std::move(record));
  }
  auto ports = scope.ports.configured_record();
  ports.boundary = 0;
  return ConfiguredGraph{
      .identity = _identity,
      .node_bundles = _node_bundles.copy_range(first, end, 0, scope.record_begin),
      .connections = GraphBuilderConnections::from_configured_connections(
          local_samples, local_events),
      .public_ports = GraphBuilderPublicPorts::from_configured_record(ports),
      .annotations = {},
      .virtual_nodes = GraphBuilderVirtualNodes::from_configured_records(local_records),
  };
}

// ---- refs and expressions -----------------------------------------------

SamplePortRef GraphBuilderState::require_sample_port(SamplePortRef const& port) const
{
  if (port.session != _session)
    details::error("sample port belongs to another builder session");
  return port;
}

EventPortRef GraphBuilderState::require_event_port(EventPortRef const& port) const
{
  if (port.session != _session)
    details::error("event port belongs to another builder session");
  return port;
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
      .detaches = std::vector<
          std::optional<ConfiguredSampleConnectionDetach>>(channels.size()),
  });
  SamplePortRef result;
  result.session = _session;
  result.channel_type = type;
  result.handle = handle;
  return result;
}

SamplePortRef GraphBuilderState::make_tiled_sample_port(
    ChannelTypeId type, std::span<SamplePortRef const> members)
{
  if (members.empty()) details::error("cannot tile an empty sample output");
  std::vector<SampleOutputChannelId> channels;
  std::vector<std::optional<ConfiguredSampleConnectionDetach>> detaches;
  channels.reserve(members.size());
  detaches.reserve(members.size());
  for (auto const& member : members) {
    if (member.session != _session
        || member.handle >= _sample_port_expressions.size())
      details::error("cannot tile sample outputs from different builder sessions");
    auto const& expression = _sample_port_expressions[member.handle];
    if (member.channel_type != ChannelTypeId::mono
        || expression.channels.size() != 1)
      details::error("each g.tile channel must be a scalar sample expression");
    channels.push_back(expression.channels.front());
    detaches.push_back(expression.detaches.front());
  }
  auto result = make_sample_port(type, channels);
  _sample_port_expressions[result.handle].detaches = std::move(detaches);
  return result;
}

SamplePortRef GraphBuilderState::select_sample_port_channel(
    SamplePortRef const& ref, size_t channel)
{
  if (ref.session != _session
      || ref.handle >= _sample_port_expressions.size())
    details::error("sample port does not belong to this builder session");
  auto const& expression = _sample_port_expressions[ref.handle];
  if (expression.channels.size() != expression.detaches.size())
    details::error("sample expression lost its per-channel detach settings");
  if (channel >= expression.channels.size())
    details::error("sample channel index is out of bounds");
  std::array<SampleOutputChannelId, 1> channels{expression.channels[channel]};
  auto detach = expression.detaches[channel];
  auto result = make_sample_port(ChannelTypeId::mono, channels);
  _sample_port_expressions[result.handle].detaches = {std::move(detach)};
  return result;
}

std::span<SampleOutputChannelId const>
GraphBuilderState::sample_port_channels(SamplePortRef const& ref) const
{
  if (ref.session != _session
      || ref.handle >= _sample_port_expressions.size()) {
    details::error("sample port does not belong to this builder session");
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
    (void)_node_bundles.resolve_event_output(
        {source.bundle, PortKind::event, source.port});
  }
  auto const handle = _event_port_expressions.size();
  _event_port_expressions.push_back({
      .type = type,
      .sources = {sources.begin(), sources.end()},
      .detaches = std::vector<
          std::optional<ConfiguredEventConnectionDetach>>(sources.size()),
  });
  EventPortRef result;
  result.session = _session;
  result.type = type;
  result.handle = handle;
  return result;
}

EventPortRef GraphBuilderState::make_event_port(
    std::span<EventPortRef const> operands)
{
  if (operands.empty())
    details::error("event fan-in expression has no source expressions");
  auto const type = operands.front().type;
  EventPortExpression expression{.type = type};
  for (auto const& operand : operands) {
    if (operand.session != _session
        || operand.handle >= _event_port_expressions.size()) {
      details::error(
          "event fan-in expression contains a port from another builder session");
    }
    auto const& source = _event_port_expressions[operand.handle];
    if (operand.type != type || source.type != type)
      details::error("event fan-in expressions have different event types");
    if (source.sources.size() != source.detaches.size())
      details::error("event fan-in source lost its per-source detach settings");
    expression.sources.insert(
        expression.sources.end(), source.sources.begin(), source.sources.end());
    expression.detaches.insert(
        expression.detaches.end(),
        source.detaches.begin(),
        source.detaches.end());
  }
  auto const handle = _event_port_expressions.size();
  _event_port_expressions.push_back(std::move(expression));
  EventPortRef result;
  result.session = _session;
  result.type = type;
  result.handle = handle;
  return result;
}

std::span<EventOutputPortId const>
GraphBuilderState::event_port_sources(EventPortRef const& ref) const
{
  if (ref.session != _session
      || ref.handle >= _event_port_expressions.size()) {
    details::error("event port does not belong to this builder session");
  }
  auto const& expression = _event_port_expressions[ref.handle];
  if (expression.type != ref.type
      || expression.sources.size() != expression.detaches.size())
    details::error("event port has an invalid semantic event type");
  return expression.sources;
}

SamplePortRef GraphBuilderState::detach_sample_port(
    SamplePortRef const& source, size_t latency,
    std::optional<Sample> initial_value) {
  if (source.session != _session
      || source.handle >= _sample_port_expressions.size())
    details::error("cannot detach a sample port from another builder session");
  if (latency < 1)
    details::error("detach loop extra latency must be at least 1");
  auto const expression = _sample_port_expressions[source.handle];
  ConfiguredSampleConnectionDetach const requested{
      .loop_extra_latency = latency,
      .initial_value_override = initial_value,
  };
  if (std::ranges::any_of(expression.detaches, [](auto const& detach) {
        return detach.has_value();
      }))
    details::error("cannot detach an already detached sample channel");
  auto detaches = expression.detaches;
  std::ranges::fill(detaches, requested);
  auto const handle = _sample_port_expressions.size();
  _sample_port_expressions.push_back({
      .channel_type = expression.channel_type,
      .channels = expression.channels,
      .detaches = std::move(detaches),
  });
  auto result = source;
  result.handle = handle;
  return result;
}

EventPortRef GraphBuilderState::detach_event_port(
    EventPortRef const& source, size_t latency) {
  if (source.session != _session
      || source.handle >= _event_port_expressions.size())
    details::error("cannot detach an event port from another builder session");
  if (latency < 1)
    details::error("detach loop extra latency must be at least 1");
  auto const expression = _event_port_expressions[source.handle];
  ConfiguredEventConnectionDetach const requested{
      .loop_extra_latency = latency,
  };
  if (std::ranges::any_of(expression.detaches, [](auto const& detach) {
        return detach.has_value();
      }))
    details::error("cannot detach an already detached event source");
  auto detaches = expression.detaches;
  std::ranges::fill(detaches, requested);
  auto const handle = _event_port_expressions.size();
  _event_port_expressions.push_back({
      .type = expression.type,
      .sources = expression.sources,
      .detaches = std::move(detaches),
  });
  auto result = source;
  result.handle = handle;
  return result;
}

void GraphBuilderState::record_configured_sample_connection(
    NodeBundlePortId target,
    SamplePortRef const& source)
{
  if (target.port_kind != PortKind::sample || !source.session
      || source.session != _session
      || source.handle >= _sample_port_expressions.size())
    details::error("invalid configured sample connection");
  auto descriptor = _node_bundles.resolve_sample_input(target);
  auto const& expression = _sample_port_expressions[source.handle];
  if (expression.channel_type != source.channel_type)
    details::error("sample port has an invalid semantic channel type");
  std::vector<ConfiguredSampleConnectionSource> configured_sources;
  configured_sources.reserve(expression.channels.size());
  for (std::size_t i = 0; i < expression.channels.size(); ++i) {
    configured_sources.push_back({
        .channel = expression.channels[i],
        .detach = expression.detaches[i],
    });
  }
  _connections.record_configured_sample_connection({
      .source_type = source.channel_type,
      .source_channels = std::move(configured_sources),
      .target_type = descriptor.config.channel_layout.channel_type,
      .target_channels = _node_bundles.sample_input_channels(target),
  });
}

void GraphBuilderState::record_configured_sample_connection(
    SampleInputChannelId target,
    SamplePortRef const& source)
{
  if (!source.session || source.session != _session
      || source.handle >= _sample_port_expressions.size())
    details::error("invalid configured sample channel connection");
  auto channels = _node_bundles.sample_input_channels(
      {target.bundle, PortKind::sample, target.port});
  if (target.channel >= channels.size() || channels[target.channel] != target)
    details::error("sample input channel does not belong to its NodeBundle port");
  auto const& expression = _sample_port_expressions[source.handle];
  if (expression.channel_type != source.channel_type)
    details::error("sample port has an invalid semantic channel type");
  std::vector<ConfiguredSampleConnectionSource> configured_sources;
  configured_sources.reserve(expression.channels.size());
  for (std::size_t i = 0; i < expression.channels.size(); ++i) {
    configured_sources.push_back({
        .channel = expression.channels[i],
        .detach = expression.detaches[i],
    });
  }
  _connections.record_configured_sample_connection({
      .source_type = source.channel_type,
      .source_channels = std::move(configured_sources),
      .target_type = ChannelTypeId::mono,
      .target_channels = {target},
  });
}

void GraphBuilderState::record_configured_event_connection(
    NodeBundlePortId target, EventPortRef const& source) {
  if (target.port_kind != PortKind::event ||
      source.session != _session || source.handle >= _event_port_expressions.size())
    details::error("invalid configured event connection");
  auto const sources = event_port_sources(source);
  if (sources.empty())
    details::error("invalid configured event connection");
  auto target_type = _node_bundles.resolve_event_input(target).config.type;
  auto const& expression = _event_port_expressions[source.handle];
  if (expression.sources.size() != expression.detaches.size())
    details::error("event expression lost its per-source detach settings");
  std::vector<ConfiguredEventConnectionSource> configured_sources;
  configured_sources.reserve(expression.sources.size());
  for (std::size_t i = 0; i < expression.sources.size(); ++i) {
    configured_sources.push_back({
        .port = expression.sources[i],
        .detach = expression.detaches[i],
    });
  }
  _connections.record_configured_event_connection({
      .source_type = source.type,
      .sources = std::move(configured_sources),
      .target_type = target_type,
      .targets = _node_bundles.event_input_ports(target),
  });
}

void GraphBuilderState::connect_sample_input(
    NodeBundlePortId target, SamplePortRef const& source)
{
  record_configured_sample_connection(target, source);
}

void GraphBuilderState::connect_sample_input(
    SampleInputChannelId target, SamplePortRef const& source)
{
  record_configured_sample_connection(target, source);
}

void GraphBuilderState::connect_sample_input(
    NodeBundlePortId target, std::span<SamplePortRef const> sources) {
  auto targets = _node_bundles.sample_input_channels(target);
  auto type = _node_bundles.resolve_sample_input(target)
                  .config.channel_layout.channel_type;
  if (sources.size() != targets.size())
    details::error(
        "sample channel source count does not match NodeBundle port layout");
  std::vector<ConfiguredSampleConnectionSource> configured_sources;
  configured_sources.reserve(sources.size());
  for (auto const& source : sources) {
    if (source.session != _session ||
        source.channel_type != ChannelTypeId::mono ||
        source.handle >= _sample_port_expressions.size())
      details::error(
          "channel-wise sample connection requires scalar sources");
    auto const& expression = _sample_port_expressions[source.handle];
    if (expression.channels.size() != 1)
      details::error(
          "channel-wise sample connection requires scalar sources");
    configured_sources.push_back({
        .channel = expression.channels.front(),
        .detach = expression.detaches.front(),
    });
  }
  _connections.record_configured_sample_connection({
      .source_type = type,
      .source_channels = std::move(configured_sources),
      .target_type = type,
      .target_channels = std::move(targets),
  });
}

void GraphBuilderState::connect_event_input(
    NodeBundlePortId target, EventPortRef const& source) {
  record_configured_event_connection(target, source);
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

NodeBundlePortId GraphBuilderState::input_port_at(
    NodeBundleHandle handle, size_t position) const {
  return _node_bundles.input_port_at(handle, position);
}

ContinuousInputConfig GraphBuilderState::sample_input_config(
    NodeBundleHandle handle, size_t port) const {
  return _node_bundles.bundle(handle).sample_input_config(port);
}

EventInputConfig GraphBuilderState::event_input_config(
    NodeBundleHandle handle, size_t port) const {
  return _node_bundles.bundle(handle).event_input_config(port);
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

EventPortRef GraphBuilderState::event_port_from_output(NodeBundlePortId port) {
  if (port.port_kind != PortKind::event)
    details::error("attempted to read an event port from a sample output");
  auto descriptor = _node_bundles.resolve_event_output(port);
  return make_event_port(
      descriptor.config.type, _node_bundles.event_output_ports(port));
}

void GraphBuilderState::apply_ttl(NodeBundleHandle handle, size_t samples) {
  _node_bundles.apply_ttl(handle, samples);
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

// ---- annotations ------------------------------------------------------

void GraphBuilderState::annotate_node(
    NodeBundleHandle handle, std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) {
  if (id.empty()) return;
  if (handle >= _node_bundles.size())
    details::error(
        "builder " + _identity.value +
        ": cannot record source info for an unknown NodeBundle");
  auto const info = make_source_info(id, file, begin, end);
  auto& infos = _node_bundles.bundle(handle).source_annotations().infos;
  if (!std::ranges::contains(infos, info)) infos.push_back(info);
  attach_virtual_bundle_member(handle, id, &info);
}

void GraphBuilderState::annotate_node_input_source_info(
    NodeBundleHandle handle, PortKind port_kind,
    std::string_view port_name, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty()) return;
  if (handle >= _node_bundles.size())
    details::error(
        "builder " + _identity.value
        + ": cannot record input-port source info for an unknown NodeBundle");
  attach_virtual_input_source_info(
      handle, id, port_kind, port_name, make_source_info(id, file, begin, end));
}

void GraphBuilderState::annotate_public_sample_input_source_info(
    SamplePortRef const& port, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || port.session != _session) return;
  auto logical = _node_bundles.sample_output_port_for_channels(
      port.channel_type, sample_port_channels(port));
  if (!logical)
    details::error("PublicSampleInputRef has no logical boundary port");
  // An open module/root scope keeps public input source infos in its port
  // metadata so finalization or import can transfer them to the subgraph.
  for (auto index = _scopes.size(); index-- > 0;) {
    auto& scope = _scopes[index];
    if (scope.ports.boundary_handle() != logical->node_bundle_handle
        || scope.kind == details::BuilderScopeKind::subgraph) {
      continue;
    }
    scope.ports.annotate_sample_input_source_info(
        logical->port_index, id, file, begin, end);
    return;
  }
  auto& boundary = _node_bundles.bundle(logical->node_bundle_handle);
  if (!boundary.is_boundary() ||
      logical->port_index >= boundary.boundary_sample_inputs().size())
    details::error(
        "PublicSampleInputRef does not belong to a valid boundary input");
  auto info = make_source_info(id, file, begin, end);
  auto& infos = boundary.source_annotations().infos;
  if (!std::ranges::contains(infos, info)) infos.push_back(std::move(info));
}

void GraphBuilderState::annotate_public_event_input_source_info(
    EventPortRef const& port, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || port.session != _session) return;
  auto const sources = event_port_sources(port);
  if (sources.size() != 1)
    details::error("PublicEventInputRef has no unique logical boundary port");
  auto source = sources.front();
  for (auto index = _scopes.size(); index-- > 0;) {
    auto& scope = _scopes[index];
    if (scope.ports.boundary_handle() != source.bundle
        || scope.kind == details::BuilderScopeKind::subgraph) {
      continue;
    }
    scope.ports.annotate_event_input_source_info(
        source.port, id, file, begin, end);
    return;
  }
  auto& boundary = _node_bundles.bundle(source.bundle);
  if (!boundary.is_boundary() ||
      source.port >= boundary.boundary_event_inputs().size())
    details::error(
        "PublicEventInputRef does not belong to a valid boundary input");
  auto info = make_source_info(id, file, begin, end);
  auto& infos = boundary.source_annotations().infos;
  if (!std::ranges::contains(infos, info)) infos.push_back(std::move(info));
}

void GraphBuilderState::annotate_sample_port_source_info(
    SamplePortRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || ref.session != _session) return;
  auto const channels = sample_port_channels(ref);
  if (channels.empty()) return;
  auto const record = virtual_record(id, "sample-port");
  _virtual_nodes.attach_sample_output(
      record, ref.channel_type, channels, make_source_info(id, file, begin, end));
}

void GraphBuilderState::annotate_event_port_source_info(
    EventPortRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end) {
  if (id.empty() || ref.session != _session) return;
  auto const sources = event_port_sources(ref);
  if (sources.empty()) return;
  auto const record = virtual_record(id, "event-port");
  _virtual_nodes.attach_event_output(
      record, ref.type, sources, make_source_info(id, file, begin, end));
}

// ---- host introspection -----------------------------------------------

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

GraphBuilderPublicContinuousPortFamilies
GraphBuilderState::public_sample_input_families() const {
  return _scopes.front().ports.sample_input_families(_node_bundles);
}

bool GraphBuilderState::public_sample_input_is_connected(size_t i) const {
  auto channels = _node_bundles.sample_output_channels(
      {_scopes.front().ports.boundary_handle(), PortKind::sample, i});
  return std::ranges::any_of(channels, [&](auto c) {
    return _connections.sample_output_is_connected(c);
  });
}

std::vector<GraphBuilderPublicEventInput>
GraphBuilderState::public_event_inputs() const {
  return _scopes.front().ports.collected_event_inputs(_node_bundles);
}

bool GraphBuilderState::public_event_input_is_connected(size_t i) const {
  auto ports = _node_bundles.event_output_ports(
      {_scopes.front().ports.boundary_handle(), PortKind::event, i});
  return std::ranges::any_of(ports, [&](auto p) {
    return _connections.event_output_is_connected(p);
  });
}

std::span<SourceInfo const>
GraphBuilderState::public_event_input_source_infos(size_t i) const {
  return _scopes.front().ports.event_input_source_infos(i);
}

GraphBuilderPublicContinuousPortFamilies
GraphBuilderState::public_sample_output_families() const {
  return _scopes.front().ports.sample_output_families(_node_bundles);
}

std::vector<GraphBuilderPublicEventOutput>
GraphBuilderState::public_event_outputs() const {
  return _scopes.front().ports.collected_event_outputs(_node_bundles);
}

ConfiguredGraph GraphBuilderState::finish() const & {
  return {
      .identity = _identity,
      .node_bundles = _node_bundles,
      .connections = _connections,
      .public_ports = _scopes.front().ports,
      .annotations = _annotations,
      .virtual_nodes = _virtual_nodes,
  };
}

ConfiguredGraph GraphBuilderState::finish() && {
  if (_scopes.size() != 1) {
    details::error("a builder graph cannot finish while a nested scope is open");
  }
  return {
      .identity = std::move(_identity),
      .node_bundles = std::move(_node_bundles),
      .connections = std::move(_connections),
      .public_ports = std::move(_scopes.front().ports),
      .annotations = std::move(_annotations),
      .virtual_nodes = std::move(_virtual_nodes),
  };
}

} // namespace iv
