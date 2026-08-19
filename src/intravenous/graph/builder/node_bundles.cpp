#include <intravenous/graph/builder/node_bundles.h>

#include <intravenous/graph/builder/topology.h>

#include <algorithm>
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
          SampleInputPortDescriptor result{
              .config = inward_input_config(payload.sample_outputs[i]),
          };
          if (payload.is_root) result.endpoints.push_back({GRAPH_ID, i});
          return result;
        } else {
          return descriptor<SampleInputPortDescriptor>(
              payload.sample_inputs, payload.sample_input_configs, i);
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
          SampleOutputPortDescriptor result{
              .config = inward_output_config(payload.sample_inputs[i]),
          };
          if (payload.sample_input_projections[i])
            result.endpoints.push_back(*payload.sample_input_projections[i]);
          else if (payload.is_root)
            result.endpoints.push_back({GRAPH_ID, i});
          return result;
        } else {
          return descriptor<SampleOutputPortDescriptor>(
              payload.sample_outputs, payload.sample_output_configs, i);
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
          EventInputPortDescriptor result{
              .config = inward_event_input_config(payload.event_outputs[i]),
          };
          if (payload.is_root) result.endpoints.push_back({GRAPH_ID, i});
          return result;
        } else {
          return descriptor<EventInputPortDescriptor>(
              payload.event_inputs, payload.event_input_configs, i);
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
          EventOutputPortDescriptor result{
              .config = inward_event_output_config(payload.event_inputs[i]),
          };
          if (payload.event_input_projections[i])
            result.endpoints.push_back(*payload.event_input_projections[i]);
          else if (payload.is_root)
            result.endpoints.push_back({GRAPH_ID, i});
          return result;
        } else {
          return descriptor<EventOutputPortDescriptor>(
              payload.event_outputs, payload.event_output_configs, i);
        }
      },
      *_payload);
}

bool NodeBundle::is_boundary() const {
  return _payload && std::holds_alternative<BoundaryNodeBundle>(*_payload);
}

std::optional<NodeBundleHandle> NodeBundle::subgraph_boundary_handle() const {
  if (!_payload) details::error("empty NodeBundle");
  auto const *subgraph = std::get_if<SubgraphNodeBundle>(&*_payload);
  if (!subgraph) return std::nullopt;
  return subgraph->boundary;
}

std::span<InputConfig const> NodeBundle::boundary_sample_inputs() const {
  if (!_payload) details::error("empty NodeBundle");
  auto const *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->sample_inputs;
}

std::span<OutputConfig const> NodeBundle::boundary_sample_outputs() const {
  if (!_payload) details::error("empty NodeBundle");
  auto const *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->sample_outputs;
}

std::span<EventInputConfig const> NodeBundle::boundary_event_inputs() const {
  if (!_payload) details::error("empty NodeBundle");
  auto const *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->event_inputs;
}

std::span<EventOutputConfig const> NodeBundle::boundary_event_outputs() const {
  if (!_payload) details::error("empty NodeBundle");
  auto const *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  if (!boundary) details::error("NodeBundle is not a boundary");
  return boundary->event_outputs;
}

size_t NodeBundle::append_boundary_sample_input(InputConfig config) {
  if (!_payload) details::error("empty NodeBundle");
  auto *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->sample_inputs.size();
  boundary->sample_inputs.push_back(std::move(config));
  boundary->sample_input_projections.emplace_back();
  return ordinal;
}

size_t NodeBundle::append_boundary_sample_input(
    InputConfig config, TopologyPortId inward_output) {
  auto const ordinal = append_boundary_sample_input(std::move(config));
  auto *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  boundary->sample_input_projections[ordinal] = inward_output;
  return ordinal;
}

void NodeBundle::set_boundary_sample_input_projection(
    size_t ordinal, TopologyPortId inward_output) {
  if (!_payload) details::error("empty NodeBundle");
  auto *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  if (!boundary) details::error("NodeBundle is not a boundary");
  if (ordinal >= boundary->sample_input_projections.size()) {
    details::error(
        "boundary sample input projection ordinal is out of bounds");
  }
  boundary->sample_input_projections[ordinal] = inward_output;
}

size_t NodeBundle::append_boundary_sample_output(OutputConfig config) {
  if (!_payload) details::error("empty NodeBundle");
  auto *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->sample_outputs.size();
  boundary->sample_outputs.push_back(std::move(config));
  return ordinal;
}

size_t NodeBundle::append_boundary_event_input(EventInputConfig config) {
  if (!_payload) details::error("empty NodeBundle");
  auto *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->event_inputs.size();
  boundary->event_inputs.push_back(std::move(config));
  boundary->event_input_projections.emplace_back();
  return ordinal;
}

size_t NodeBundle::append_boundary_event_input(
    EventInputConfig config, TopologyPortId inward_output) {
  auto const ordinal = append_boundary_event_input(std::move(config));
  auto *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  boundary->event_input_projections[ordinal] = inward_output;
  return ordinal;
}

void NodeBundle::set_boundary_event_input_projection(
    size_t ordinal, TopologyPortId inward_output) {
  if (!_payload) details::error("empty NodeBundle");
  auto *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  if (!boundary) details::error("NodeBundle is not a boundary");
  if (ordinal >= boundary->event_input_projections.size()) {
    details::error(
        "boundary event input projection ordinal is out of bounds");
  }
  boundary->event_input_projections[ordinal] = inward_output;
}

size_t NodeBundle::append_boundary_event_output(EventOutputConfig config) {
  if (!_payload) details::error("empty NodeBundle");
  auto *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  if (!boundary) details::error("NodeBundle is not a boundary");
  auto const ordinal = boundary->event_outputs.size();
  boundary->event_outputs.push_back(std::move(config));
  return ordinal;
}

void NodeBundle::clear_boundary_event_outputs() {
  if (!_payload) details::error("empty NodeBundle");
  auto *boundary = std::get_if<BoundaryNodeBundle>(&*_payload);
  if (!boundary) details::error("NodeBundle is not a boundary");
  boundary->event_outputs.clear();
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
  if (is_boundary()) return "Boundary";
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
      [](auto const &payload) {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
          return payload.sample_outputs.size();
        else
          return payload.sample_input_configs.size();
      },
      *_payload);
}
size_t NodeBundle::sample_output_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [](auto const &payload) {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
          return payload.sample_inputs.size();
        else
          return payload.sample_output_configs.size();
      },
      *_payload);
}
size_t NodeBundle::event_input_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [](auto const &payload) {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
          return payload.event_outputs.size();
        else
          return payload.event_input_configs.size();
      },
      *_payload);
}
size_t NodeBundle::event_output_count() const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [](auto const &payload) {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
          return payload.event_inputs.size();
        else
          return payload.event_output_configs.size();
      },
      *_payload);
}
size_t NodeBundle::sample_input_index(std::string_view name) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
          return index_for_name(payload.sample_outputs, name);
        else
          return index_for_name(payload.sample_input_configs, name);
      },
      *_payload);
}
size_t NodeBundle::sample_output_index(std::string_view name) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
          return index_for_name(payload.sample_inputs, name);
        else
          return index_for_name(payload.sample_output_configs, name);
      },
      *_payload);
}
size_t NodeBundle::event_input_index(std::string_view name) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
          return index_for_name(payload.event_outputs, name);
        else
          return index_for_name(payload.event_input_configs, name);
      },
      *_payload);
}
size_t NodeBundle::event_output_index(std::string_view name) const {
  if (!_payload) details::error("empty NodeBundle");
  return std::visit(
      [&](auto const &payload) {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>)
          return index_for_name(payload.event_inputs, name);
        else
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
        } else if constexpr (requires { payload.node; }) {
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
void NodeBundle::import_into(size_t offset, size_t bundle_offset) {
  if (!_payload) details::error("empty NodeBundle");
  std::visit(
      [&](auto &payload) {
        using Bundle = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<Bundle, SubgraphNodeBundle>) {
          payload.boundary += bundle_offset;
        } else if constexpr (std::is_same_v<Bundle, TiledNodeBundle>) {
          for (auto &member : payload.member_bundles) {
            member += bundle_offset;
          }
        } else if constexpr (std::is_same_v<Bundle, BoundaryNodeBundle>) {
          // Imported boundaries are semantic interfaces inside the parent.
          // Their old root/scope topology projections belong to the child
          // topology and must not masquerade as parent endpoints.
          payload.is_root = false;
          for (auto &projection : payload.sample_input_projections)
            projection.reset();
          for (auto &projection : payload.event_input_projections)
            projection.reset();
        }
        if constexpr (requires { payload.nodes; }) {
          for (auto &node : payload.nodes) node += offset;
        } else if constexpr (requires { payload.node; }) {
          payload.node += offset;
        }
        if constexpr (requires { payload.sample_input_configs; }) {
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
        }
      },
      *_payload);
  _virtual_node_handles.clear();
}
std::vector<size_t> &NodeBundle::virtual_node_handles() { return _virtual_node_handles; }
std::vector<size_t> const &NodeBundle::virtual_node_handles() const { return _virtual_node_handles; }
NodeSourceAnnotations &NodeBundle::source_annotations() { return _source_annotations; }
NodeSourceAnnotations const &NodeBundle::source_annotations() const { return _source_annotations; }

NodeBundleHandle GraphBuilderNodeBundles::append_boundary() {
  auto const handle = _bundles.size();
  _bundles.push_back(NodeBundle(NodeBundle::BoundaryNodeBundle{.is_root = true}));
  return handle;
}

NodeBundleHandle GraphBuilderNodeBundles::append_scope_boundary() {
  auto const handle = _bundles.size();
  _bundles.push_back(NodeBundle(NodeBundle::BoundaryNodeBundle{}));
  return handle;
}

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
    std::span<NodeBundleHandle const> member_bundles,
    ChannelLayout promoted) {
  if (nodes.empty()) details::error("a tiled NodeBundle requires at least one ConcreteNode");
  if (member_bundles.size() != nodes.size()) {
    details::error("a tiled NodeBundle requires one member bundle per concrete node");
  }
  auto const &first = topology.concrete_node(nodes.front());
  NodeBundle::TiledNodeBundle p{
    .nodes = {nodes.begin(), nodes.end()},
    .member_bundles = {member_bundles.begin(), member_bundles.end()},
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
    GraphBuilderTopology const &topology, size_t node, NodeBundleHandle boundary) {
  if (boundary >= _bundles.size() || !_bundles[boundary].is_boundary())
    details::error("a SubgraphNodeBundle requires a BoundaryNodeBundle");

  auto const &n = topology.subgraph_node(node);
  auto const &interface = _bundles[boundary];
  auto const sample_inputs = interface.boundary_sample_inputs();
  auto const sample_outputs = interface.boundary_sample_outputs();
  auto const event_inputs = interface.boundary_event_inputs();
  auto const event_outputs = interface.boundary_event_outputs();

  if (n.inputs().size() != sample_inputs.size()
      || n.outputs().size() != sample_outputs.size()
      || n.event_inputs().size() != event_inputs.size()
      || n.event_outputs().size() != event_outputs.size()) {
    details::error(
        "SubgraphNode topology interface does not match its BoundaryNodeBundle");
  }

  NodeBundle::SubgraphNodeBundle p{
    .boundary = boundary,
    .node = node,
    .sample_inputs{}, .sample_outputs{},
    .event_inputs{}, .event_outputs{},
    .sample_input_configs = {sample_inputs.begin(), sample_inputs.end()},
    .sample_output_configs = {sample_outputs.begin(), sample_outputs.end()},
    .event_input_configs = {event_inputs.begin(), event_inputs.end()},
    .event_output_configs = {event_outputs.begin(), event_outputs.end()},
  };
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

std::vector<SampleInputChannelId>
GraphBuilderNodeBundles::sample_input_channels(NodeBundlePortId address) const {
  auto const descriptor = resolve_sample_input(address);
  auto const count = channel_count(descriptor.config.channel_layout.channel_type);
  std::vector<SampleInputChannelId> result;
  result.reserve(count);
  for (size_t channel = 0; channel < count; ++channel) {
    result.push_back(SampleInputChannelId{
        .bundle = address.node_bundle_handle,
        .port = address.port_ordinal,
        .channel = channel,
    });
  }
  return result;
}

std::vector<SampleOutputChannelId>
GraphBuilderNodeBundles::sample_output_channels(NodeBundlePortId address) const {
  auto const descriptor = resolve_sample_output(address);
  auto const count = channel_count(descriptor.config.channel_layout.channel_type);
  std::vector<SampleOutputChannelId> result;
  result.reserve(count);
  for (size_t channel = 0; channel < count; ++channel) {
    result.push_back(SampleOutputChannelId{
        .bundle = address.node_bundle_handle,
        .port = address.port_ordinal,
        .channel = channel,
    });
  }
  return result;
}

namespace {
template<class Channel>
std::vector<Channel> channels_for_topology_projection(
    TopologyPortId topology_port,
    std::vector<TopologyPortId> const& endpoints,
    std::vector<Channel> channels) {
  if (endpoints.empty()) {
    details::error("NodeBundle sample port has no topology endpoint");
  }
  if (endpoints.size() == 1) {
    if (endpoints.front() != topology_port) {
      details::error("topology port does not belong to the resolved NodeBundle port");
    }
    return channels;
  }
  if (endpoints.size() != channels.size()) {
    details::error(
        "NodeBundle sample topology projection does not match its semantic channels");
  }
  auto const it = std::find(endpoints.begin(), endpoints.end(), topology_port);
  if (it == endpoints.end()) {
    details::error("topology port does not belong to the resolved NodeBundle port");
  }
  auto const channel = static_cast<size_t>(it - endpoints.begin());
  return {channels[channel]};
}

template<class Port>
std::vector<Port> event_ports_for_topology_projection(
    TopologyPortId topology_port,
    std::vector<TopologyPortId> const& endpoints,
    std::vector<Port> ports) {
  if (endpoints.empty()) {
    details::error("NodeBundle event port has no topology endpoint");
  }
  if (endpoints.size() == 1) {
    if (endpoints.front() != topology_port) {
      details::error("topology port does not belong to the resolved NodeBundle port");
    }
    return ports;
  }
  if (endpoints.size() != ports.size()) {
    details::error(
        "NodeBundle event topology projection does not match its semantic ports");
  }
  auto const it = std::find(endpoints.begin(), endpoints.end(), topology_port);
  if (it == endpoints.end()) {
    details::error("topology port does not belong to the resolved NodeBundle port");
  }
  auto const ordinal = static_cast<size_t>(it - endpoints.begin());
  return {ports[ordinal]};
}
} // namespace

std::vector<SampleInputChannelId>
GraphBuilderNodeBundles::sample_input_channels_for_topology_port(
    TopologyPortId topology_port) const {
  auto const bundle_handle = bundle_for_concrete_node(topology_port.node);
  NodeBundlePortId const logical{
      bundle_handle, PortKind::sample, topology_port.port};
  auto const descriptor = resolve_sample_input(logical);
  return channels_for_topology_projection(
      topology_port, descriptor.endpoints, sample_input_channels(logical));
}

std::vector<SampleOutputChannelId>
GraphBuilderNodeBundles::sample_output_channels_for_topology_port(
    TopologyPortId topology_port) const {
  auto const bundle_handle = bundle_for_concrete_node(topology_port.node);
  NodeBundlePortId const logical{
      bundle_handle, PortKind::sample, topology_port.port};
  auto const descriptor = resolve_sample_output(logical);
  return channels_for_topology_projection(
      topology_port, descriptor.endpoints, sample_output_channels(logical));
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

std::vector<EventInputPortId>
GraphBuilderNodeBundles::event_input_ports(NodeBundlePortId address) const {
  auto const descriptor = resolve_event_input(address);
  auto const &logical_bundle = bundle(address.node_bundle_handle);
  if (logical_bundle._payload) {
    if (auto const *tiled =
            std::get_if<NodeBundle::TiledNodeBundle>(&*logical_bundle._payload)) {
      std::vector<EventInputPortId> result;
      result.reserve(tiled->member_bundles.size());
      for (auto const member : tiled->member_bundles) {
        NodeBundlePortId const member_port{
            member, PortKind::event, address.port_ordinal};
        if (resolve_event_input(member_port).config.type != descriptor.config.type) {
          details::error(
              "tiled NodeBundle member event input type does not match promoted port");
        }
        result.push_back(EventInputPortId{
            .bundle = member,
            .port = address.port_ordinal,
        });
      }
      return result;
    }
  }
  return {EventInputPortId{
      .bundle = address.node_bundle_handle,
      .port = address.port_ordinal,
  }};
}

std::vector<EventOutputPortId>
GraphBuilderNodeBundles::event_output_ports(NodeBundlePortId address) const {
  auto const descriptor = resolve_event_output(address);
  auto const &logical_bundle = bundle(address.node_bundle_handle);
  if (logical_bundle._payload) {
    if (auto const *tiled =
            std::get_if<NodeBundle::TiledNodeBundle>(&*logical_bundle._payload)) {
      std::vector<EventOutputPortId> result;
      result.reserve(tiled->member_bundles.size());
      for (auto const member : tiled->member_bundles) {
        NodeBundlePortId const member_port{
            member, PortKind::event, address.port_ordinal};
        if (resolve_event_output(member_port).config.type != descriptor.config.type) {
          details::error(
              "tiled NodeBundle member event output type does not match promoted port");
        }
        result.push_back(EventOutputPortId{
            .bundle = member,
            .port = address.port_ordinal,
        });
      }
      return result;
    }
  }
  return {EventOutputPortId{
      .bundle = address.node_bundle_handle,
      .port = address.port_ordinal,
  }};
}

std::vector<EventInputPortId>
GraphBuilderNodeBundles::event_input_ports_for_topology_port(
    TopologyPortId topology_port) const {
  auto const bundle_handle = bundle_for_concrete_node(topology_port.node);
  NodeBundlePortId const logical{
      bundle_handle, PortKind::event, topology_port.port};
  auto const descriptor = resolve_event_input(logical);
  return event_ports_for_topology_projection(
      topology_port, descriptor.endpoints, event_input_ports(logical));
}

std::vector<EventOutputPortId>
GraphBuilderNodeBundles::event_output_ports_for_topology_port(
    TopologyPortId topology_port) const {
  auto const bundle_handle = bundle_for_concrete_node(topology_port.node);
  NodeBundlePortId const logical{
      bundle_handle, PortKind::event, topology_port.port};
  auto const descriptor = resolve_event_output(logical);
  return event_ports_for_topology_projection(
      topology_port, descriptor.endpoints, event_output_ports(logical));
}

NodeBundleHandle GraphBuilderNodeBundles::bundle_for_concrete_node(size_t node) const { if (node >= _bundle_by_concrete_node.size() || _bundle_by_concrete_node[node] == GRAPH_ID) details::error("concrete node has no NodeBundle"); return _bundle_by_concrete_node[node]; }
size_t GraphBuilderNodeBundles::size() const { return _bundles.size(); }
void GraphBuilderNodeBundles::import_child(GraphBuilderNodeBundles const &child, size_t offset) { auto const bundle_offset = _bundles.size(); for (auto bundle : child._bundles) { bundle.import_into(offset, bundle_offset); auto handle = _bundles.size(); bundle.for_each_topology_node([&](size_t node) { if (_bundle_by_concrete_node.size() <= node) _bundle_by_concrete_node.resize(node + 1, GRAPH_ID); _bundle_by_concrete_node[node] = handle; }); _bundles.push_back(std::move(bundle)); } }
} // namespace iv
