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
  std::vector<ConcretePortId> sample_inputs, sample_outputs{};
  std::vector<ConcretePortId> event_inputs, event_outputs{};
  std::vector<ChannelLayout> sample_input_layouts, sample_output_layouts{};
  std::vector<std::string> sample_input_names, sample_output_names{};
  std::vector<std::string> event_input_names, event_output_names{};
};

struct TiledNodeBundle {
  std::vector<size_t> nodes{};
  std::vector<std::vector<ConcretePortId>> sample_inputs, sample_outputs{};
  std::vector<std::vector<ConcretePortId>> event_inputs, event_outputs{};
  std::vector<ChannelLayout> sample_input_layouts, sample_output_layouts{};
  std::vector<std::string> sample_input_names, sample_output_names{};
  std::vector<std::string> event_input_names, event_output_names{};
};

struct SubgraphNodeBundle {
  size_t node{};
  std::vector<ConcretePortId> sample_inputs, sample_outputs{};
  std::vector<ConcretePortId> event_inputs, event_outputs{};
  std::vector<ChannelLayout> sample_input_layouts, sample_output_layouts{};
  std::vector<std::string> sample_input_names, sample_output_names{};
  std::vector<std::string> event_input_names, event_output_names{};
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

template <class Payload, auto Member>
void each_ports(void const *payload, size_t ordinal,
                std::function<void(ConcretePortId)> const &fn) {
  auto const &values = static_cast<Payload const *>(payload)->*Member;
  if (ordinal >= values.size()) details::error("NodeBundle port ordinal is out of bounds");
  if constexpr (std::is_same_v<Payload, TiledNodeBundle>)
    for (auto const port : values[ordinal]) fn(port);
  else
    fn(values[ordinal]);
}
template <class Payload>
ChannelLayout input_layout(void const *payload, size_t ordinal) {
  auto const &layouts = static_cast<Payload const *>(payload)->sample_input_layouts;
  if (ordinal >= layouts.size()) details::error("NodeBundle sample input ordinal is out of bounds");
  return layouts[ordinal];
}
template <class Payload>
ChannelLayout output_layout(void const *payload, size_t ordinal) {
  auto const &layouts = static_cast<Payload const *>(payload)->sample_output_layouts;
  if (ordinal >= layouts.size()) details::error("NodeBundle sample output ordinal is out of bounds");
  return layouts[ordinal];
}
template <class Payload, auto Member> size_t count(void const *payload) {
  return (static_cast<Payload const *>(payload)->*Member).size();
}
template <class Payload, auto Member>
size_t index_for_name(void const *payload, std::string_view name) {
  auto const &names = static_cast<Payload const *>(payload)->*Member;
  std::optional<size_t> result;
  for (size_t ordinal = 0; ordinal < names.size(); ++ordinal) {
    if (names[ordinal] != name) continue;
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
  void (*sample_input)(void const *, size_t, std::function<void(ConcretePortId)> const &);
  void (*sample_output)(void const *, size_t, std::function<void(ConcretePortId)> const &);
  void (*event_input)(void const *, size_t, std::function<void(ConcretePortId)> const &);
  void (*event_output)(void const *, size_t, std::function<void(ConcretePortId)> const &);
  ChannelLayout (*input_layout)(void const *, size_t);
  ChannelLayout (*output_layout)(void const *, size_t);
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
      .sample_input = each_ports<Payload, &Payload::sample_inputs>,
      .sample_output = each_ports<Payload, &Payload::sample_outputs>,
      .event_input = each_ports<Payload, &Payload::event_inputs>,
      .event_output = each_ports<Payload, &Payload::event_outputs>,
      .input_layout = input_layout<Payload>, .output_layout = output_layout<Payload>,
      .sample_input_count = count<Payload, &Payload::sample_inputs>,
      .sample_output_count = count<Payload, &Payload::sample_outputs>,
      .event_input_count = count<Payload, &Payload::event_inputs>,
      .event_output_count = count<Payload, &Payload::event_outputs>,
      .sample_input_index = index_for_name<Payload, &Payload::sample_input_names>,
      .sample_output_index = index_for_name<Payload, &Payload::sample_output_names>,
      .event_input_index = index_for_name<Payload, &Payload::event_input_names>,
      .event_output_index = index_for_name<Payload, &Payload::event_output_names>,
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
void NodeBundle::for_each_sample_input(size_t i, std::function<void(TopologyPortId)> const &fn) const { _ops->sample_input(_payload, i, fn); }
void NodeBundle::for_each_sample_output(size_t i, std::function<void(TopologyPortId)> const &fn) const { _ops->sample_output(_payload, i, fn); }
void NodeBundle::for_each_event_input(size_t i, std::function<void(TopologyPortId)> const &fn) const { _ops->event_input(_payload, i, fn); }
void NodeBundle::for_each_event_output(size_t i, std::function<void(TopologyPortId)> const &fn) const { _ops->event_output(_payload, i, fn); }
ChannelLayout NodeBundle::sample_input_layout(size_t i) const { return _ops->input_layout(_payload, i); }
ChannelLayout NodeBundle::sample_output_layout(size_t i) const { return _ops->output_layout(_payload, i); }

InputConfig NodeBundle::sample_input_config(
    GraphBuilderTopology const &topology, size_t ordinal) const {
  std::optional<TopologyPortId> port;
  for_each_sample_input(ordinal, [&](TopologyPortId value) {
    if (!port) port = value;
  });
  if (!port) details::error("NodeBundle sample input has no topology endpoint");
  auto config = topology.is_subgraph_node(port->node)
      ? topology.subgraph_node(port->node).inputs().at(port->port)
      : topology.concrete_node(port->node).inputs().at(port->port);
  config.channel_layout = sample_input_layout(ordinal);
  return config;
}

OutputConfig NodeBundle::sample_output_config(
    GraphBuilderTopology const &topology, size_t ordinal) const {
  std::optional<TopologyPortId> port;
  for_each_sample_output(ordinal, [&](TopologyPortId value) {
    if (!port) port = value;
  });
  if (!port) details::error("NodeBundle sample output has no topology endpoint");
  auto config = topology.is_subgraph_node(port->node)
      ? topology.subgraph_node(port->node).outputs().at(port->port)
      : topology.concrete_node(port->node).outputs().at(port->port);
  config.channel_layout = sample_output_layout(ordinal);
  return config;
}

EventInputConfig NodeBundle::event_input_config(
    GraphBuilderTopology const &topology, size_t ordinal) const {
  std::optional<TopologyPortId> port;
  for_each_event_input(ordinal, [&](TopologyPortId value) {
    if (!port) port = value;
  });
  if (!port) details::error("NodeBundle event input has no topology endpoint");
  return topology.is_subgraph_node(port->node)
      ? topology.subgraph_node(port->node).event_inputs().at(port->port)
      : topology.concrete_node(port->node).event_inputs().at(port->port);
}

EventOutputConfig NodeBundle::event_output_config(
    GraphBuilderTopology const &topology, size_t ordinal) const {
  std::optional<TopologyPortId> port;
  for_each_event_output(ordinal, [&](TopologyPortId value) {
    if (!port) port = value;
  });
  if (!port) details::error("NodeBundle event output has no topology endpoint");
  return topology.is_subgraph_node(port->node)
      ? topology.subgraph_node(port->node).event_outputs().at(port->port)
      : topology.concrete_node(port->node).event_outputs().at(port->port);
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

NodeBundleHandle GraphBuilderNodeBundles::append_concrete(GraphBuilderTopology const &topology, size_t node) {
  auto const &n = topology.concrete_node(node); ConcreteNodeBundle p{.node = node};
  for (size_t i = 0; i < n.inputs().size(); ++i) { p.sample_inputs.push_back({node, i}); p.sample_input_layouts.push_back(n.inputs()[i].channel_layout); p.sample_input_names.push_back(n.inputs()[i].name); }
  for (size_t i = 0; i < n.outputs().size(); ++i) { p.sample_outputs.push_back({node, i}); p.sample_output_layouts.push_back(n.outputs()[i].channel_layout); p.sample_output_names.push_back(n.outputs()[i].name); }
  for (size_t i = 0; i < n.event_inputs().size(); ++i) { p.event_inputs.push_back({node, i}); p.event_input_names.push_back(n.event_inputs()[i].name); }
  for (size_t i = 0; i < n.event_outputs().size(); ++i) { p.event_outputs.push_back({node, i}); p.event_output_names.push_back(n.event_outputs()[i].name); }
  auto handle = _bundles.size(); _bundles.push_back(make_bundle(std::move(p))); if (_bundle_by_concrete_node.size() <= node) _bundle_by_concrete_node.resize(node + 1, GRAPH_ID); _bundle_by_concrete_node[node] = handle; return handle;
}

NodeBundleHandle GraphBuilderNodeBundles::append_tiled(GraphBuilderTopology const &topology, std::span<size_t const> nodes, ChannelLayout promoted) {
  if (nodes.empty()) details::error("a tiled NodeBundle requires at least one ConcreteNode");
  auto const &first = topology.concrete_node(nodes.front()); TiledNodeBundle p{.nodes = {nodes.begin(), nodes.end()}};
  auto append_sample = [&](auto const &configs, auto &dest, auto &layouts) {
    for (size_t port = 0; port < configs.size(); ++port) { if (configs[port].channel_layout.channel_type != ChannelTypeId::mono) details::error("a tiled NodeBundle requires mono concrete sample ports"); std::vector<ConcretePortId> ports; for (auto node : nodes) ports.push_back({node, port}); dest.push_back(std::move(ports)); layouts.push_back({promoted.channel_type, configs[port].channel_layout.sample_layout}); }
  };
  append_sample(first.inputs(), p.sample_inputs, p.sample_input_layouts); append_sample(first.outputs(), p.sample_outputs, p.sample_output_layouts);
  for (auto const &config : first.inputs()) p.sample_input_names.push_back(config.name);
  for (auto const &config : first.outputs()) p.sample_output_names.push_back(config.name);
  for (size_t port = 0; port < first.event_inputs().size(); ++port) { std::vector<ConcretePortId> ports; for (auto node : nodes) ports.push_back({node, port}); p.event_inputs.push_back(std::move(ports)); p.event_input_names.push_back(first.event_inputs()[port].name); }
  for (size_t port = 0; port < first.event_outputs().size(); ++port) { std::vector<ConcretePortId> ports; for (auto node : nodes) ports.push_back({node, port}); p.event_outputs.push_back(std::move(ports)); p.event_output_names.push_back(first.event_outputs()[port].name); }
  auto handle = _bundles.size(); _bundles.push_back(make_bundle(std::move(p))); for (auto node : nodes) { if (_bundle_by_concrete_node.size() <= node) _bundle_by_concrete_node.resize(node + 1, GRAPH_ID); _bundle_by_concrete_node[node] = handle; } return handle;
}

NodeBundleHandle GraphBuilderNodeBundles::append_subgraph(GraphBuilderTopology const &topology, size_t node) {
  auto const &n = topology.subgraph_node(node); SubgraphNodeBundle p{.node = node};
  for (size_t i = 0; i < n.inputs().size(); ++i) { p.sample_inputs.push_back({node, i}); p.sample_input_layouts.push_back(n.inputs()[i].channel_layout); p.sample_input_names.push_back(n.inputs()[i].name); }
  for (size_t i = 0; i < n.outputs().size(); ++i) { p.sample_outputs.push_back({node, i}); p.sample_output_layouts.push_back(n.outputs()[i].channel_layout); p.sample_output_names.push_back(n.outputs()[i].name); }
  for (size_t i = 0; i < n.event_inputs().size(); ++i) { p.event_inputs.push_back({node, i}); p.event_input_names.push_back(n.event_inputs()[i].name); } for (size_t i = 0; i < n.event_outputs().size(); ++i) { p.event_outputs.push_back({node, i}); p.event_output_names.push_back(n.event_outputs()[i].name); }
  auto handle = _bundles.size(); _bundles.push_back(make_bundle(std::move(p))); if (_bundle_by_concrete_node.size() <= node) _bundle_by_concrete_node.resize(node + 1, GRAPH_ID); _bundle_by_concrete_node[node] = handle; return handle;
}
NodeBundle const &GraphBuilderNodeBundles::bundle(NodeBundleHandle h) const { return _bundles.at(h); }
NodeBundle &GraphBuilderNodeBundles::bundle(NodeBundleHandle h) { return _bundles.at(h); }
NodeBundleHandle GraphBuilderNodeBundles::bundle_for_concrete_node(size_t node) const { if (node >= _bundle_by_concrete_node.size() || _bundle_by_concrete_node[node] == GRAPH_ID) details::error("concrete node has no NodeBundle"); return _bundle_by_concrete_node[node]; }
size_t GraphBuilderNodeBundles::size() const { return _bundles.size(); }
void GraphBuilderNodeBundles::import_child(GraphBuilderNodeBundles const &child, size_t offset) { for (auto bundle : child._bundles) { bundle.import_into(offset); auto handle = _bundles.size(); bundle.for_each_topology_node([&](size_t node) { if (_bundle_by_concrete_node.size() <= node) _bundle_by_concrete_node.resize(node + 1, GRAPH_ID); _bundle_by_concrete_node[node] = handle; }); _bundles.push_back(std::move(bundle)); } }
} // namespace iv
