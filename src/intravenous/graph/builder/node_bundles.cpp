#include <intravenous/graph/builder/node_bundles.h>

#include <intravenous/graph/builder/topology.h>

#include <utility>
#include <type_traits>
#include <optional>
#include <string>

namespace iv {
namespace {
template <class Descriptor, class Ports, class Configs>
Descriptor descriptor(Ports const &ports, Configs const &configs,
                      size_t ordinal) {
  if (ordinal >= ports.size() || ordinal >= configs.size())
    details::error("NodeBundle port ordinal is out of bounds");

  Descriptor result{.config = configs[ordinal]};
  if constexpr (requires { result.endpoints = ports[ordinal]; })
    result.endpoints = ports[ordinal];
  else
    result.endpoints.push_back(ports[ordinal]);
  return result;
}

template <class Configs>
size_t index_for_name(Configs const &configs, std::string_view name) {
  std::optional<size_t> result;
  for (size_t ordinal = 0; ordinal < configs.size(); ++ordinal) {
    if (configs[ordinal].name != name) continue;
    if (result) details::error("NodeBundle port name '" + std::string(name) + "' is ambiguous");
    result = ordinal;
  }
  if (!result) details::error("NodeBundle port name '" + std::string(name) + "' does not exist");
  return *result;
}
} // namespace

NodeBundle::NodeBundle(ConcreteNodeBundle payload)
    : _payload(Payload{std::move(payload)}) {}
NodeBundle::NodeBundle(TiledNodeBundle payload)
    : _payload(Payload{std::move(payload)}) {}
NodeBundle::NodeBundle(SubgraphNodeBundle payload)
    : _payload(Payload{std::move(payload)}) {}

SampleInputPortDescriptor NodeBundle::sample_input_descriptor(size_t i) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        return descriptor<SampleInputPortDescriptor>(
            payload.sample_inputs, payload.sample_input_configs, i);
      },
      *_payload);
}
SampleOutputPortDescriptor NodeBundle::sample_output_descriptor(size_t i) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        return descriptor<SampleOutputPortDescriptor>(
            payload.sample_outputs, payload.sample_output_configs, i);
      },
      *_payload);
}
EventInputPortDescriptor NodeBundle::event_input_descriptor(size_t i) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        return descriptor<EventInputPortDescriptor>(
            payload.event_inputs, payload.event_input_configs, i);
      },
      *_payload);
}
EventOutputPortDescriptor NodeBundle::event_output_descriptor(size_t i) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        return descriptor<EventOutputPortDescriptor>(
            payload.event_outputs, payload.event_output_configs, i);
      },
      *_payload);
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

size_t NodeBundle::sample_input_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [](auto const &payload) { return payload.sample_input_configs.size(); },
      *_payload);
}
size_t NodeBundle::sample_output_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [](auto const &payload) { return payload.sample_output_configs.size(); },
      *_payload);
}
size_t NodeBundle::event_input_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [](auto const &payload) { return payload.event_input_configs.size(); },
      *_payload);
}
size_t NodeBundle::event_output_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [](auto const &payload) { return payload.event_output_configs.size(); },
      *_payload);
}
size_t NodeBundle::sample_input_index(std::string_view name) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        return index_for_name(payload.sample_input_configs, name);
      },
      *_payload);
}
size_t NodeBundle::sample_output_index(std::string_view name) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        return index_for_name(payload.sample_output_configs, name);
      },
      *_payload);
}
size_t NodeBundle::event_input_index(std::string_view name) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        return index_for_name(payload.event_input_configs, name);
      },
      *_payload);
}
size_t NodeBundle::event_output_index(std::string_view name) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        return index_for_name(payload.event_output_configs, name);
      },
      *_payload);
}
void NodeBundle::for_each_topology_node(
    std::function<void(size_t)> const &fn) const {
  if (!_payload) details::error("empty NodeBundle");
  std::visit(
      [&](auto const &payload) {
        if constexpr (requires { payload.nodes; }) {
          for (auto const node : payload.nodes) fn(node);
        } else {
          fn(payload.node);
        }
      },
      *_payload);
}
void NodeBundle::for_each_concrete_node(
    std::function<void(size_t)> const &fn) const {
  if (!_payload) details::error("empty NodeBundle");
  std::visit(
      [&](auto const &payload) {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (Bundle::contains_concrete_nodes) {
          if constexpr (requires { payload.nodes; }) {
            for (auto const node : payload.nodes) fn(node);
          } else {
            fn(payload.node);
          }
        }
      },
      *_payload);
}
size_t NodeBundle::single_concrete_node() const {
  std::optional<size_t> node;
  for_each_concrete_node([&](size_t value) {
    if (node) details::error("this operation requires a bundle with one concrete node");
    node = value;
  });
  if (!node) details::error("this operation requires a bundle with one concrete node");
  return *node;
}
void NodeBundle::import_into(size_t offset) {
  if (!_payload) details::error("empty NodeBundle");
  std::visit(
      [&](auto &payload) {
        if constexpr (requires { payload.nodes; }) {
          for (auto &node : payload.nodes) node += offset;
        } else {
          payload.node += offset;
        }
        auto offset_ports = [offset](auto &ports) {
          for (auto &port_or_ports : ports) {
            if constexpr (requires { port_or_ports.node; }) {
              port_or_ports.node += offset;
            } else {
              for (auto &port : port_or_ports) port.node += offset;
            }
          }
        };
        offset_ports(payload.sample_inputs);
        offset_ports(payload.sample_outputs);
        offset_ports(payload.event_inputs);
        offset_ports(payload.event_outputs);
      },
      *_payload);
  _virtual_node_handles.clear();
}
std::vector<size_t> &NodeBundle::virtual_node_handles() { return _virtual_node_handles; }
std::vector<size_t> const &NodeBundle::virtual_node_handles() const { return _virtual_node_handles; }
NodeSourceAnnotations &NodeBundle::source_annotations() { return _source_annotations; }
NodeSourceAnnotations const &NodeBundle::source_annotations() const { return _source_annotations; }

NodeBundleHandle GraphBuilderNodeBundles::append_concrete(
    GraphBuilderTopology const &topology, size_t node) {
  auto const &n = topology.concrete_node(node);
  NodeBundle::ConcreteNodeBundle p{
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
  _bundles.push_back(NodeBundle(std::move(p)));
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
  NodeBundle::TiledNodeBundle p{
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
  _bundles.push_back(NodeBundle(std::move(p)));
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
  NodeBundle::SubgraphNodeBundle p{
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
  _bundles.push_back(NodeBundle(std::move(p)));
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
