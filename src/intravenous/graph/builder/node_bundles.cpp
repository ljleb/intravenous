#include <intravenous/graph/builder/node_bundles.h>

#include <algorithm>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

namespace iv {
namespace {
OutputConfig inward_output_config(InputConfig const &config) {
  return OutputConfig{
      .name = config.name,
      .channel_layout = config.channel_layout,
      .history = config.history,
  };
}

InputConfig inward_input_config(OutputConfig const &config) {
  return InputConfig{
      .name = config.name,
      .channel_layout = config.channel_layout,
      .history = config.history,
  };
}

EventOutputConfig inward_event_output_config(EventInputConfig const &config) {
  return EventOutputConfig{.name = config.name, .type = config.type};
}

EventInputConfig inward_event_input_config(EventOutputConfig const &config) {
  return EventInputConfig{.name = config.name, .type = config.type};
}

template <class Configs>
size_t index_for_name(Configs const &configs, std::string_view name) {
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
Descriptor descriptor(Configs const &configs, size_t ordinal) {
  if (ordinal >= configs.size()) {
    details::error("NodeBundle port ordinal is out of bounds");
  }
  return Descriptor{.config = configs[ordinal]};
}
} // namespace

NodeBundle::NodeBundle(ConcreteNodeBundle payload)
    : _payload(Payload{std::move(payload)}) {}
NodeBundle::NodeBundle(TiledNodeBundle payload)
    : _payload(Payload{std::move(payload)}) {}
NodeBundle::NodeBundle(BoundaryNodeBundle payload)
    : _payload(Payload{std::move(payload)}) {}
NodeBundle::NodeBundle(SubgraphNodeBundle payload)
    : _payload(Payload{std::move(payload)}) {}

SampleInputPortDescriptor NodeBundle::sample_input_descriptor(size_t i) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) -> SampleInputPortDescriptor {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) {
          if (i >= payload.sample_outputs.size())
            details::error("NodeBundle port ordinal is out of bounds");
          return {.config = inward_input_config(payload.sample_outputs[i])};
        } else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
          return descriptor<SampleInputPortDescriptor>(
              payload.ports.sample_inputs, i);
        } else {
          return descriptor<SampleInputPortDescriptor>(
              payload.sample_input_configs, i);
        }
      },
      *_payload);
}

SampleOutputPortDescriptor NodeBundle::sample_output_descriptor(size_t i) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) -> SampleOutputPortDescriptor {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) {
          if (i >= payload.sample_inputs.size())
            details::error("NodeBundle port ordinal is out of bounds");
          return {.config = inward_output_config(payload.sample_inputs[i])};
        } else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
          return descriptor<SampleOutputPortDescriptor>(
              payload.ports.sample_outputs, i);
        } else {
          return descriptor<SampleOutputPortDescriptor>(
              payload.sample_output_configs, i);
        }
      },
      *_payload);
}

EventInputPortDescriptor NodeBundle::event_input_descriptor(size_t i) const {
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
        } else {
          return descriptor<EventInputPortDescriptor>(
              payload.event_input_configs, i);
        }
      },
      *_payload);
}

EventOutputPortDescriptor NodeBundle::event_output_descriptor(size_t i) const {
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
        } else {
          return descriptor<EventOutputPortDescriptor>(
              payload.event_output_configs, i);
        }
      },
      *_payload);
}

bool NodeBundle::is_concrete() const {
  return _payload && std::holds_alternative<ConcreteNodeBundle>(*_payload);
}
bool NodeBundle::is_tiled() const {
  return _payload && std::holds_alternative<TiledNodeBundle>(*_payload);
}
bool NodeBundle::is_boundary() const {
  return _payload && std::holds_alternative<BoundaryNodeBundle>(*_payload);
}
bool NodeBundle::is_subgraph() const {
  return _payload && std::holds_alternative<SubgraphNodeBundle>(*_payload);
}

std::optional<NodeBundleHandle> NodeBundle::subgraph_boundary_handle() const {
  if (!_payload) details::error("empty NodeBundle");
  auto const *subgraph = std::get_if<SubgraphNodeBundle>(&*_payload);
  return subgraph ? std::optional<NodeBundleHandle>(subgraph->boundary)
                  : std::nullopt;
}

std::span<NodeBundleHandle const> NodeBundle::tiled_members() const {
  if (!_payload) details::error("empty NodeBundle");
  auto const *tiled = std::get_if<TiledNodeBundle>(&*_payload);
  if (!tiled) details::error("NodeBundle is not tiled");
  return tiled->member_bundles;
}

size_t NodeBundle::subgraph_child_begin() const {
  auto const *subgraph = _payload ? std::get_if<SubgraphNodeBundle>(&*_payload) : nullptr;
  if (!subgraph) details::error("NodeBundle is not a subgraph");
  return subgraph->child_begin;
}
size_t NodeBundle::subgraph_child_count() const {
  auto const *subgraph = _payload ? std::get_if<SubgraphNodeBundle>(&*_payload) : nullptr;
  if (!subgraph) details::error("NodeBundle is not a subgraph");
  return subgraph->child_count;
}
std::string_view NodeBundle::subgraph_kind() const {
  auto const *subgraph = _payload ? std::get_if<SubgraphNodeBundle>(&*_payload) : nullptr;
  if (!subgraph) details::error("NodeBundle is not a subgraph");
  return subgraph->kind;
}

std::span<InputConfig const> NodeBundle::boundary_sample_inputs() const {
  auto const *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->sample_inputs;
}
std::span<OutputConfig const> NodeBundle::boundary_sample_outputs() const {
  auto const *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->sample_outputs;
}
std::span<EventInputConfig const> NodeBundle::boundary_event_inputs() const {
  auto const *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->event_inputs;
}
std::span<EventOutputConfig const> NodeBundle::boundary_event_outputs() const {
  auto const *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->event_outputs;
}

size_t NodeBundle::append_boundary_sample_input(InputConfig config) {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->sample_inputs.size();
  boundary->sample_inputs.push_back(std::move(config));
  return ordinal;
}
size_t NodeBundle::append_boundary_sample_output(OutputConfig config) {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->sample_outputs.size();
  boundary->sample_outputs.push_back(std::move(config));
  return ordinal;
}
size_t NodeBundle::append_boundary_event_input(EventInputConfig config) {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->event_inputs.size();
  boundary->event_inputs.push_back(std::move(config));
  return ordinal;
}
size_t NodeBundle::append_boundary_event_output(EventOutputConfig config) {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->event_outputs.size();
  boundary->event_outputs.push_back(std::move(config));
  return ordinal;
}
void NodeBundle::clear_boundary_event_outputs() {
  auto *boundary = _payload ? std::get_if<BoundaryNodeBundle>(&*_payload) : nullptr;
  if (!boundary) details::error("NodeBundle is not a boundary");
  boundary->event_outputs.clear();
}

ChannelLayout NodeBundle::sample_input_layout(size_t i) const {
  return sample_input_descriptor(i).config.channel_layout;
}
ChannelLayout NodeBundle::sample_output_layout(size_t i) const {
  return sample_output_descriptor(i).config.channel_layout;
}
InputConfig NodeBundle::sample_input_config(size_t i) const {
  return sample_input_descriptor(i).config;
}
OutputConfig NodeBundle::sample_output_config(size_t i) const {
  return sample_output_descriptor(i).config;
}
EventInputConfig NodeBundle::event_input_config(size_t i) const {
  return event_input_descriptor(i).config;
}
EventOutputConfig NodeBundle::event_output_config(size_t i) const {
  return event_output_descriptor(i).config;
}

std::string_view NodeBundle::type_identity() const {
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

NodePorts const &NodeBundle::concrete_ports() const {
  auto const *concrete = _payload ? std::get_if<ConcreteNodeBundle>(&*_payload) : nullptr;
  if (!concrete) details::error("NodeBundle is not a concrete node bundle");
  return concrete->ports;
}

size_t NodeBundle::sample_input_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit([](auto const &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) return payload.sample_outputs.size();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) return payload.ports.sample_inputs.size();
    else return payload.sample_input_configs.size();
  }, *_payload);
}
size_t NodeBundle::sample_output_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit([](auto const &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) return payload.sample_inputs.size();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) return payload.ports.sample_outputs.size();
    else return payload.sample_output_configs.size();
  }, *_payload);
}
size_t NodeBundle::event_input_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit([](auto const &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) return payload.event_outputs.size();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) return payload.ports.event_input_configs.size();
    else return payload.event_input_configs.size();
  }, *_payload);
}
size_t NodeBundle::event_output_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit([](auto const &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) return payload.event_inputs.size();
    else if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) return payload.ports.event_output_configs.size();
    else return payload.event_output_configs.size();
  }, *_payload);
}

size_t NodeBundle::sample_input_index(std::string_view name) const {
  std::vector<InputConfig> configs;
  configs.reserve(sample_input_count());
  for (size_t i = 0; i < sample_input_count(); ++i) configs.push_back(sample_input_config(i));
  return index_for_name(configs, name);
}
size_t NodeBundle::sample_output_index(std::string_view name) const {
  std::vector<OutputConfig> configs;
  configs.reserve(sample_output_count());
  for (size_t i = 0; i < sample_output_count(); ++i) configs.push_back(sample_output_config(i));
  return index_for_name(configs, name);
}
size_t NodeBundle::event_input_index(std::string_view name) const {
  std::vector<EventInputConfig> configs;
  configs.reserve(event_input_count());
  for (size_t i = 0; i < event_input_count(); ++i) configs.push_back(event_input_config(i));
  return index_for_name(configs, name);
}
size_t NodeBundle::event_output_index(std::string_view name) const {
  std::vector<EventOutputConfig> configs;
  configs.reserve(event_output_count());
  for (size_t i = 0; i < event_output_count(); ++i) configs.push_back(event_output_config(i));
  return index_for_name(configs, name);
}

void NodeBundle::import_into(size_t node_bundle_offset,
                             size_t detach_id_offset) {
  if (!_payload) details::error("empty NodeBundle");
  std::visit([&](auto &payload) {
    using Bundle = std::remove_cvref_t<decltype(payload)>;
    if constexpr (std::is_same_v<Bundle, ConcreteNodeBundle>) {
      auto factory = std::move(payload.materialization.factory);
      payload.materialization.factory =
          [factory = std::move(factory), detach_id_offset](size_t offset) mutable {
            return factory(offset + detach_id_offset);
          };
    } else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
      for (auto &member : payload.member_bundles) member += node_bundle_offset;
    } else if constexpr (std::is_same_v<Bundle, SubgraphNodeBundle>) {
      payload.boundary += node_bundle_offset;
      payload.child_begin += node_bundle_offset;
    }
  }, *_payload);
  _virtual_node_handles.clear();
}

std::vector<size_t> &NodeBundle::virtual_node_handles() { return _virtual_node_handles; }
std::vector<size_t> const &NodeBundle::virtual_node_handles() const { return _virtual_node_handles; }
NodeSourceAnnotations &NodeBundle::source_annotations() { return _source_annotations; }
NodeSourceAnnotations const &NodeBundle::source_annotations() const { return _source_annotations; }

NodeBundleHandle GraphBuilderNodeBundles::append_boundary() {
  auto const handle = _bundles.size();
  _bundles.push_back(NodeBundle(NodeBundle::BoundaryNodeBundle{}));
  return handle;
}
NodeBundleHandle GraphBuilderNodeBundles::append_scope_boundary() {
  return append_boundary();
}

NodeBundleHandle GraphBuilderNodeBundles::append_tiled(
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

NodeBundleHandle GraphBuilderNodeBundles::append_subgraph(
    NodeBundleHandle boundary, size_t child_begin, size_t child_count,
    std::string_view kind) {
  auto const &boundary_bundle = bundle(boundary);
  if (!boundary_bundle.is_boundary()) details::error("subgraph boundary is not a BoundaryNodeBundle");
  if (child_begin + child_count > _bundles.size()) details::error("subgraph child bundle range is out of bounds");

  NodeBundle::SubgraphNodeBundle payload{
      .boundary = boundary,
      .child_begin = child_begin,
      .child_count = child_count,
      .kind = std::string(kind),
      .type_identity = NodeTypeIdentity{.value = "lowered-subgraph:" + std::string(kind)},
  };
  payload.sample_input_configs.assign(boundary_bundle.boundary_sample_inputs().begin(),
                                      boundary_bundle.boundary_sample_inputs().end());
  payload.sample_output_configs.assign(boundary_bundle.boundary_sample_outputs().begin(),
                                       boundary_bundle.boundary_sample_outputs().end());
  payload.event_input_configs.assign(boundary_bundle.boundary_event_inputs().begin(),
                                     boundary_bundle.boundary_event_inputs().end());
  payload.event_output_configs.assign(boundary_bundle.boundary_event_outputs().begin(),
                                      boundary_bundle.boundary_event_outputs().end());

  auto const handle = _bundles.size();
  _bundles.push_back(NodeBundle(std::move(payload)));
  return handle;
}

NodeBundle const &GraphBuilderNodeBundles::bundle(NodeBundleHandle handle) const {
  if (handle >= _bundles.size()) details::error("NodeBundle handle is out of bounds");
  return _bundles[handle];
}
NodeBundle &GraphBuilderNodeBundles::bundle(NodeBundleHandle handle) {
  if (handle >= _bundles.size()) details::error("NodeBundle handle is out of bounds");
  return _bundles[handle];
}

SampleInputPortDescriptor GraphBuilderNodeBundles::resolve_sample_input(NodeBundlePortId id) const {
  if (id.port_kind != PortKind::sample) details::error("sample input address has event kind");
  return bundle(id.node_bundle_handle).sample_input_descriptor(id.port_ordinal);
}
SampleOutputPortDescriptor GraphBuilderNodeBundles::resolve_sample_output(NodeBundlePortId id) const {
  if (id.port_kind != PortKind::sample) details::error("sample output address has event kind");
  return bundle(id.node_bundle_handle).sample_output_descriptor(id.port_ordinal);
}
EventInputPortDescriptor GraphBuilderNodeBundles::resolve_event_input(NodeBundlePortId id) const {
  if (id.port_kind != PortKind::event) details::error("event input address has sample kind");
  return bundle(id.node_bundle_handle).event_input_descriptor(id.port_ordinal);
}
EventOutputPortDescriptor GraphBuilderNodeBundles::resolve_event_output(NodeBundlePortId id) const {
  if (id.port_kind != PortKind::event) details::error("event output address has sample kind");
  return bundle(id.node_bundle_handle).event_output_descriptor(id.port_ordinal);
}

std::vector<SampleInputChannelId> GraphBuilderNodeBundles::sample_input_channels(NodeBundlePortId id) const {
  auto const type = resolve_sample_input(id).config.channel_layout.channel_type;
  std::vector<SampleInputChannelId> result;
  for (size_t channel = 0; channel < channel_count(type); ++channel)
    result.push_back({id.node_bundle_handle, id.port_ordinal, channel});
  return result;
}
std::vector<SampleOutputChannelId> GraphBuilderNodeBundles::sample_output_channels(NodeBundlePortId id) const {
  auto const type = resolve_sample_output(id).config.channel_layout.channel_type;
  std::vector<SampleOutputChannelId> result;
  for (size_t channel = 0; channel < channel_count(type); ++channel)
    result.push_back({id.node_bundle_handle, id.port_ordinal, channel});
  return result;
}
std::optional<NodeBundlePortId> GraphBuilderNodeBundles::sample_output_port_for_channels(
    ChannelTypeId type, std::span<SampleOutputChannelId const> channels) const {
  for (NodeBundleHandle handle = 0; handle < _bundles.size(); ++handle) {
    auto const &candidate = bundle(handle);
    for (size_t port = 0; port < candidate.sample_output_count(); ++port) {
      NodeBundlePortId const id{handle, PortKind::sample, port};
      if (resolve_sample_output(id).config.channel_layout.channel_type != type) continue;
      auto const candidate_channels = sample_output_channels(id);
      if (std::ranges::equal(candidate_channels, channels)) return id;
    }
  }
  return std::nullopt;
}
std::vector<EventInputPortId> GraphBuilderNodeBundles::event_input_ports(NodeBundlePortId id) const {
  (void)resolve_event_input(id);
  return {{id.node_bundle_handle, id.port_ordinal}};
}
std::vector<EventOutputPortId> GraphBuilderNodeBundles::event_output_ports(NodeBundlePortId id) const {
  (void)resolve_event_output(id);
  return {{id.node_bundle_handle, id.port_ordinal}};
}

NodePorts const &GraphBuilderNodeBundles::typed_ports(NodeBundleHandle handle) const {
  auto const &b = bundle(handle);
  if (b.is_concrete()) return b.concrete_ports();
  if (b.is_tiled()) {
    auto const members = b.tiled_members();
    if (members.empty()) details::error("tiled NodeBundle has no members");
    return bundle(members.front()).concrete_ports();
  }
  details::error("typed NodeRef does not refer to a concrete or tiled bundle");
}

NodeBundleHandle GraphBuilderNodeBundles::tiled_member(NodeBundleHandle handle,
                                                        size_t channel) const {
  auto const members = bundle(handle).tiled_members();
  if (channel >= members.size()) details::error("tiled NodeBundle channel is out of bounds");
  return members[channel];
}

ConcreteNode GraphBuilderNodeBundles::lowered_concrete(NodeBundleHandle handle) const {
  auto const &b = bundle(handle);
  auto const *payload = b._payload ? std::get_if<NodeBundle::ConcreteNodeBundle>(&*b._payload) : nullptr;
  if (!payload) details::error("NodeBundle is not concrete");
  return ConcreteNode{
      .ports = payload->ports,
      .materialization = payload->materialization,
      .lifetime = payload->lifetime,
      .type_identity = payload->type_identity,
  };
}

SemanticSubgraphInfo GraphBuilderNodeBundles::subgraph_info(NodeBundleHandle handle) const {
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

size_t GraphBuilderNodeBundles::size() const { return _bundles.size(); }

void GraphBuilderNodeBundles::apply_ttl(NodeBundleHandle handle,
                                        size_t ttl_samples) {
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

size_t GraphBuilderNodeBundles::import_child(
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
