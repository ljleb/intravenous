#include <intravenous/graph/builder/node_bundles.h>

#include <intravenous/graph/builder/topology.h>

#include <utility>
#include <type_traits>
#include <optional>
#include <string>

namespace iv {
namespace {
struct ConcreteNodeBundle {
  size_t node{};
  std::vector<TopologyPortId> sample_inputs, sample_outputs{};
  std::vector<TopologyPortId> event_inputs, event_outputs{};
  std::vector<InputConfig> sample_input_configs{};
  std::vector<OutputConfig> sample_output_configs{};
  std::vector<EventInputConfig> event_input_configs{};
  std::vector<EventOutputConfig> event_output_configs{};
};

struct TiledNodeBundle {
  std::vector<size_t> nodes{};
  std::vector<std::vector<TopologyPortId>> sample_inputs, sample_outputs{};
  std::vector<std::vector<TopologyPortId>> event_inputs, event_outputs{};
  std::vector<InputConfig> sample_input_configs{};
  std::vector<OutputConfig> sample_output_configs{};
  std::vector<EventInputConfig> event_input_configs{};
  std::vector<EventOutputConfig> event_output_configs{};
};

struct SubgraphNodeBundle {
  size_t node{};
  std::vector<TopologyPortId> sample_inputs, sample_outputs{};
  std::vector<TopologyPortId> event_inputs, event_outputs{};
  std::vector<InputConfig> sample_input_configs{};
  std::vector<OutputConfig> sample_output_configs{};
  std::vector<EventInputConfig> event_input_configs{};
  std::vector<EventOutputConfig> event_output_configs{};
};

template <class Payload> void destroy(void *payload) { delete static_cast<Payload *>(payload); }
template <class Payload> void *clone(void const *payload) {
  return new Payload(*static_cast<Payload const *>(payload));
}
template <class Payload> void import(void *payload, size_t offset) {
  auto &p = *static_cast<Payload *>(payload);
  if constexpr (std::is_same_v<Payload, TiledNodeBundle>) {
    for (auto &node : p.nodes) node += offset;
    auto offset_ports = [offset](auto &ports) {
      for (auto &port_list : ports) for (auto &port : port_list) port.node += offset;
    };
    offset_ports(p.sample_inputs); offset_ports(p.sample_outputs);
    offset_ports(p.event_inputs); offset_ports(p.event_outputs);
  } else {
    p.node += offset;
    auto offset_ports = [offset](auto &ports) { for (auto &port : ports) port.node += offset; };
    offset_ports(p.sample_inputs); offset_ports(p.sample_outputs);
    offset_ports(p.event_inputs); offset_ports(p.event_outputs);
  }
}

template <class Descriptor, class Payload, auto PortsMember, auto ConfigsMember>
Descriptor descriptor(void const *payload, size_t ordinal) {
  auto const &p = *static_cast<Payload const *>(payload);
  auto const &ports = p.*PortsMember;
  auto const &configs = p.*ConfigsMember;
  if (ordinal >= ports.size() || ordinal >= configs.size())
    details::error("NodeBundle port ordinal is out of bounds");

  Descriptor result{.config = configs[ordinal]};
  if constexpr (std::is_same_v<Payload, TiledNodeBundle>)
    result.endpoints = ports[ordinal];
  else
    result.endpoints.push_back(ports[ordinal]);
  return result;
}

template <class Payload, auto ConfigsMember> size_t count(void const *payload) {
  return (static_cast<Payload const *>(payload)->*ConfigsMember).size();
}
template <class Payload, auto ConfigsMember>
size_t index_for_name(void const *payload, std::string_view name) {
  auto const &configs = static_cast<Payload const *>(payload)->*ConfigsMember;
  std::optional<size_t> result;
  for (size_t ordinal = 0; ordinal < configs.size(); ++ordinal) {
    if (configs[ordinal].name != name) continue;
    if (result) details::error("NodeBundle port name '" + std::string(name) + "' is ambiguous");
    result = ordinal;
  }
  if (!result) details::error("NodeBundle port name '" + std::string(name) + "' does not exist");
  return *result;
}
template <class Payload> void each_node(void const *payload, std::function<void(size_t)> const &fn) {
  auto const &p = *static_cast<Payload const *>(payload);
  if constexpr (std::is_same_v<Payload, TiledNodeBundle>) for (auto const node : p.nodes) fn(node);
  else fn(p.node);
}
template <class Payload> void each_concrete_node(void const *payload, std::function<void(size_t)> const &fn) {
  if constexpr (!std::is_same_v<Payload, SubgraphNodeBundle>) each_node<Payload>(payload, fn);
}

template <class Payload> NodeBundle::Ops const &ops_for();
} // namespace

struct NodeBundle::Ops {
  void (*destroy)(void *);
  void *(*clone)(void const *);
  SampleInputPortDescriptor (*sample_input_descriptor)(void const *, size_t);
  SampleOutputPortDescriptor (*sample_output_descriptor)(void const *, size_t);
  EventInputPortDescriptor (*event_input_descriptor)(void const *, size_t);
  EventOutputPortDescriptor (*event_output_descriptor)(void const *, size_t);
  size_t (*sample_input_count)(void const *);
  size_t (*sample_output_count)(void const *);
  size_t (*event_input_count)(void const *);
  size_t (*event_output_count)(void const *);
  size_t (*sample_input_index)(void const *, std::string_view);
  size_t (*sample_output_index)(void const *, std::string_view);
  size_t (*event_input_index)(void const *, std::string_view);
  size_t (*event_output_index)(void const *, std::string_view);
  void (*each_node)(void const *, std::function<void(size_t)> const &);
  void (*each_concrete_node)(void const *, std::function<void(size_t)> const &);
  void (*import_into)(void *, size_t);
};

namespace {
template <class Payload> NodeBundle::Ops const &ops_for() {
  static NodeBundle::Ops const ops{
      .destroy = destroy<Payload>, .clone = clone<Payload>,
      .sample_input_descriptor = descriptor<SampleInputPortDescriptor, Payload,
          &Payload::sample_inputs, &Payload::sample_input_configs>,
      .sample_output_descriptor = descriptor<SampleOutputPortDescriptor, Payload,
          &Payload::sample_outputs, &Payload::sample_output_configs>,
      .event_input_descriptor = descriptor<EventInputPortDescriptor, Payload,
          &Payload::event_inputs, &Payload::event_input_configs>,
      .event_output_descriptor = descriptor<EventOutputPortDescriptor, Payload,
          &Payload::event_outputs, &Payload::event_output_configs>,
      .sample_input_count = count<Payload, &Payload::sample_input_configs>,
      .sample_output_count = count<Payload, &Payload::sample_output_configs>,
      .event_input_count = count<Payload, &Payload::event_input_configs>,
      .event_output_count = count<Payload, &Payload::event_output_configs>,
      .sample_input_index = index_for_name<Payload, &Payload::sample_input_configs>,
      .sample_output_index = index_for_name<Payload, &Payload::sample_output_configs>,
      .event_input_index = index_for_name<Payload, &Payload::event_input_configs>,
      .event_output_index = index_for_name<Payload, &Payload::event_output_configs>,
      .each_node = each_node<Payload>, .each_concrete_node = each_concrete_node<Payload>,
      .import_into = import<Payload>};
  return ops;
}

template <class Payload> NodeBundle make_bundle(Payload payload) {
  return NodeBundle(new Payload(std::move(payload)), &ops_for<Payload>());
}
} // namespace

NodeBundle::NodeBundle() = default;
NodeBundle::NodeBundle(void *payload, Ops const *ops) : _payload(payload), _ops(ops) {}
NodeBundle::NodeBundle(NodeBundle const &other)
    : _payload(other._payload ? other._ops->clone(other._payload) : nullptr), _ops(other._ops),
      _virtual_node_handles(other._virtual_node_handles), _source_annotations(other._source_annotations) {}
NodeBundle::NodeBundle(NodeBundle &&other) noexcept
    : _payload(std::exchange(other._payload, nullptr)),
      _ops(std::exchange(other._ops, nullptr)),
      _virtual_node_handles(std::move(other._virtual_node_handles)),
      _source_annotations(std::move(other._source_annotations)) {}
NodeBundle &NodeBundle::operator=(NodeBundle const &other) { NodeBundle copy(other); *this = std::move(copy); return *this; }
NodeBundle &NodeBundle::operator=(NodeBundle &&other) noexcept {
  if (this != &other) { if (_payload) _ops->destroy(_payload); _payload = std::exchange(other._payload, nullptr); _ops = std::exchange(other._ops, nullptr); _virtual_node_handles = std::move(other._virtual_node_handles); _source_annotations = std::move(other._source_annotations); }
  return *this;
}
NodeBundle::~NodeBundle() { if (_payload) _ops->destroy(_payload); }
SampleInputPortDescriptor NodeBundle::sample_input_descriptor(size_t i) const {
  return _ops->sample_input_descriptor(_payload, i);
}
SampleOutputPortDescriptor NodeBundle::sample_output_descriptor(size_t i) const {
  return _ops->sample_output_descriptor(_payload, i);
}
EventInputPortDescriptor NodeBundle::event_input_descriptor(size_t i) const {
  return _ops->event_input_descriptor(_payload, i);
}
EventOutputPortDescriptor NodeBundle::event_output_descriptor(size_t i) const {
  return _ops->event_output_descriptor(_payload, i);
}

void NodeBundle::for_each_sample_input(
    size_t i, std::function<void(TopologyPortId)> const &fn) const {
  for (auto const port : sample_input_descriptor(i).endpoints) fn(port);
}
void NodeBundle::for_each_sample_output(
    size_t i, std::function<void(TopologyPortId)> const &fn) const {
  for (auto const port : sample_output_descriptor(i).endpoints) fn(port);
}
void NodeBundle::for_each_event_input(
    size_t i, std::function<void(TopologyPortId)> const &fn) const {
  for (auto const port : event_input_descriptor(i).endpoints) fn(port);
}
void NodeBundle::for_each_event_output(
    size_t i, std::function<void(TopologyPortId)> const &fn) const {
  for (auto const port : event_output_descriptor(i).endpoints) fn(port);
}
ChannelLayout NodeBundle::sample_input_layout(size_t i) const {
  return sample_input_descriptor(i).config.channel_layout;
}
ChannelLayout NodeBundle::sample_output_layout(size_t i) const {
  return sample_output_descriptor(i).config.channel_layout;
}

InputConfig NodeBundle::sample_input_config(
    GraphBuilderTopology const &, size_t ordinal) const {
  return sample_input_descriptor(ordinal).config;
}

OutputConfig NodeBundle::sample_output_config(
    GraphBuilderTopology const &, size_t ordinal) const {
  return sample_output_descriptor(ordinal).config;
}

EventInputConfig NodeBundle::event_input_config(
    GraphBuilderTopology const &, size_t ordinal) const {
  return event_input_descriptor(ordinal).config;
}

EventOutputConfig NodeBundle::event_output_config(
    GraphBuilderTopology const &, size_t ordinal) const {
  return event_output_descriptor(ordinal).config;
}

std::string_view NodeBundle::type_identity(GraphBuilderTopology const &topology) const {
  std::optional<size_t> node;
  for_each_topology_node([&](size_t value) {
    if (!node) node = value;
  });
  if (!node) details::error("NodeBundle has no topology node");
  return topology.is_subgraph_node(*node)
      ? std::string_view(topology.subgraph_node(*node).type_identity.value)
      : std::string_view(topology.concrete_node(*node).type_identity.value);
}

size_t NodeBundle::sample_input_count() const { return _ops->sample_input_count(_payload); }
size_t NodeBundle::sample_output_count() const { return _ops->sample_output_count(_payload); }
size_t NodeBundle::event_input_count() const { return _ops->event_input_count(_payload); }
size_t NodeBundle::event_output_count() const { return _ops->event_output_count(_payload); }
size_t NodeBundle::sample_input_index(std::string_view name) const { return _ops->sample_input_index(_payload, name); }
size_t NodeBundle::sample_output_index(std::string_view name) const { return _ops->sample_output_index(_payload, name); }
size_t NodeBundle::event_input_index(std::string_view name) const { return _ops->event_input_index(_payload, name); }
size_t NodeBundle::event_output_index(std::string_view name) const { return _ops->event_output_index(_payload, name); }
void NodeBundle::for_each_topology_node(std::function<void(size_t)> const &fn) const { _ops->each_node(_payload, fn); }
void NodeBundle::for_each_concrete_node(std::function<void(size_t)> const &fn) const { _ops->each_concrete_node(_payload, fn); }
size_t NodeBundle::single_concrete_node() const {
  std::optional<size_t> node;
  for_each_concrete_node([&](size_t value) {
    if (node) details::error("this operation requires a bundle with one concrete node");
    node = value;
  });
  if (!node) details::error("this operation requires a bundle with one concrete node");
  return *node;
}
void NodeBundle::import_into(size_t offset) { _ops->import_into(_payload, offset); _virtual_node_handles.clear(); }
std::vector<size_t> &NodeBundle::virtual_node_handles() { return _virtual_node_handles; }
std::vector<size_t> const &NodeBundle::virtual_node_handles() const { return _virtual_node_handles; }
NodeSourceAnnotations &NodeBundle::source_annotations() { return _source_annotations; }
NodeSourceAnnotations const &NodeBundle::source_annotations() const { return _source_annotations; }

NodeBundleHandle GraphBuilderNodeBundles::append_concrete(
    GraphBuilderTopology const &topology, size_t node) {
  auto const &n = topology.concrete_node(node);
  ConcreteNodeBundle p{
    .node = node,
    .sample_inputs{}, .sample_outputs{},
    .event_inputs{}, .event_outputs{},
    .sample_input_configs{},
    .sample_output_configs{},
    .event_input_configs{},
    .event_output_configs{},
  };
  p.sample_input_configs = n.inputs();
  p.sample_output_configs = n.outputs();
  p.event_input_configs = n.event_inputs();
  p.event_output_configs = n.event_outputs();
  for (size_t i = 0; i < p.sample_input_configs.size(); ++i)
    p.sample_inputs.push_back({node, i});
  for (size_t i = 0; i < p.sample_output_configs.size(); ++i)
    p.sample_outputs.push_back({node, i});
  for (size_t i = 0; i < p.event_input_configs.size(); ++i)
    p.event_inputs.push_back({node, i});
  for (size_t i = 0; i < p.event_output_configs.size(); ++i)
    p.event_outputs.push_back({node, i});
  auto handle = _bundles.size();
  _bundles.push_back(make_bundle(std::move(p)));
  if (_bundle_by_concrete_node.size() <= node)
    _bundle_by_concrete_node.resize(node + 1, GRAPH_ID);
  _bundle_by_concrete_node[node] = handle;
  return handle;
}

NodeBundleHandle GraphBuilderNodeBundles::append_tiled(
    GraphBuilderTopology const &topology, std::span<size_t const> nodes,
    ChannelLayout promoted) {
  if (nodes.empty()) details::error("a tiled NodeBundle requires at least one ConcreteNode");
  auto const &first = topology.concrete_node(nodes.front());
  TiledNodeBundle p{
    .nodes = {nodes.begin(), nodes.end()},
    .sample_inputs{}, .sample_outputs{},
    .event_inputs{}, .event_outputs{},
    .sample_input_configs{},
    .sample_output_configs{},
    .event_input_configs{},
    .event_output_configs{},
  };

  auto append_sample = [&](auto const &configs, auto &dest_ports,
                           auto &dest_configs) {
    for (size_t port = 0; port < configs.size(); ++port) {
      if (configs[port].channel_layout.channel_type != ChannelTypeId::mono)
        details::error("a tiled NodeBundle requires mono concrete sample ports");
      std::vector<TopologyPortId> ports;
      for (auto node : nodes) ports.push_back({node, port});
      dest_ports.push_back(std::move(ports));
      auto config = configs[port];
      config.channel_layout = {promoted.channel_type,
                               config.channel_layout.sample_layout};
      dest_configs.push_back(std::move(config));
    }
  };
  append_sample(first.inputs(), p.sample_inputs, p.sample_input_configs);
  append_sample(first.outputs(), p.sample_outputs, p.sample_output_configs);

  p.event_input_configs = first.event_inputs();
  p.event_output_configs = first.event_outputs();
  for (size_t port = 0; port < p.event_input_configs.size(); ++port) {
    std::vector<TopologyPortId> ports;
    for (auto node : nodes) ports.push_back({node, port});
    p.event_inputs.push_back(std::move(ports));
  }
  for (size_t port = 0; port < p.event_output_configs.size(); ++port) {
    std::vector<TopologyPortId> ports;
    for (auto node : nodes) ports.push_back({node, port});
    p.event_outputs.push_back(std::move(ports));
  }

  auto handle = _bundles.size();
  _bundles.push_back(make_bundle(std::move(p)));
  for (auto node : nodes) {
    if (_bundle_by_concrete_node.size() <= node)
      _bundle_by_concrete_node.resize(node + 1, GRAPH_ID);
    _bundle_by_concrete_node[node] = handle;
  }
  return handle;
}

NodeBundleHandle GraphBuilderNodeBundles::append_subgraph(
    GraphBuilderTopology const &topology, size_t node) {
  auto const &n = topology.subgraph_node(node);
  SubgraphNodeBundle p{
    .node = node,
    .sample_inputs{}, .sample_outputs{},
    .event_inputs{}, .event_outputs{},
    .sample_input_configs{},
    .sample_output_configs{},
    .event_input_configs{},
    .event_output_configs{},
  };
  p.sample_input_configs = n.inputs();
  p.sample_output_configs = n.outputs();
  p.event_input_configs = n.event_inputs();
  p.event_output_configs = n.event_outputs();
  for (size_t i = 0; i < p.sample_input_configs.size(); ++i)
    p.sample_inputs.push_back({node, i});
  for (size_t i = 0; i < p.sample_output_configs.size(); ++i)
    p.sample_outputs.push_back({node, i});
  for (size_t i = 0; i < p.event_input_configs.size(); ++i)
    p.event_inputs.push_back({node, i});
  for (size_t i = 0; i < p.event_output_configs.size(); ++i)
    p.event_outputs.push_back({node, i});
  auto handle = _bundles.size();
  _bundles.push_back(make_bundle(std::move(p)));
  if (_bundle_by_concrete_node.size() <= node)
    _bundle_by_concrete_node.resize(node + 1, GRAPH_ID);
  _bundle_by_concrete_node[node] = handle;
  return handle;
}
NodeBundle const &GraphBuilderNodeBundles::bundle(NodeBundleHandle h) const { return _bundles.at(h); }
NodeBundle &GraphBuilderNodeBundles::bundle(NodeBundleHandle h) { return _bundles.at(h); }

SampleInputPortDescriptor GraphBuilderNodeBundles::resolve_sample_input(
    NodeBundlePortId address) const {
  if (address.port_kind != PortKind::sample)
    details::error("attempted to resolve a sample input from an event NodeBundle port");
  return bundle(address.node_bundle_handle).sample_input_descriptor(address.port_ordinal);
}

SampleOutputPortDescriptor GraphBuilderNodeBundles::resolve_sample_output(
    NodeBundlePortId address) const {
  if (address.port_kind != PortKind::sample)
    details::error("attempted to resolve a sample output from an event NodeBundle port");
  return bundle(address.node_bundle_handle).sample_output_descriptor(address.port_ordinal);
}

EventInputPortDescriptor GraphBuilderNodeBundles::resolve_event_input(
    NodeBundlePortId address) const {
  if (address.port_kind != PortKind::event)
    details::error("attempted to resolve an event input from a sample NodeBundle port");
  return bundle(address.node_bundle_handle).event_input_descriptor(address.port_ordinal);
}

EventOutputPortDescriptor GraphBuilderNodeBundles::resolve_event_output(
    NodeBundlePortId address) const {
  if (address.port_kind != PortKind::event)
    details::error("attempted to resolve an event output from a sample NodeBundle port");
  return bundle(address.node_bundle_handle).event_output_descriptor(address.port_ordinal);
}

NodeBundleHandle GraphBuilderNodeBundles::bundle_for_concrete_node(size_t node) const { if (node >= _bundle_by_concrete_node.size() || _bundle_by_concrete_node[node] == GRAPH_ID) details::error("concrete node has no NodeBundle"); return _bundle_by_concrete_node[node]; }
size_t GraphBuilderNodeBundles::size() const { return _bundles.size(); }
void GraphBuilderNodeBundles::import_child(GraphBuilderNodeBundles const &child, size_t offset) { for (auto bundle : child._bundles) { bundle.import_into(offset); auto handle = _bundles.size(); bundle.for_each_topology_node([&](size_t node) { if (_bundle_by_concrete_node.size() <= node) _bundle_by_concrete_node.resize(node + 1, GRAPH_ID); _bundle_by_concrete_node[node] = handle; }); _bundles.push_back(std::move(bundle)); } }
} // namespace iv
