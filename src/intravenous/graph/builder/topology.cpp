#include <intravenous/graph/builder/topology.h>

namespace iv {
size_t GraphBuilderTopology::node_count() const { return _nodes.size(); }

ConcreteNode &GraphBuilderTopology::concrete_node(size_t index) {
  return std::get<ConcreteNode>(_nodes.at(index));
}
ConcreteNode const &GraphBuilderTopology::concrete_node(size_t index) const {
  return std::get<ConcreteNode>(_nodes.at(index));
}
SubgraphNode &GraphBuilderTopology::subgraph_node(size_t index) {
  return std::get<SubgraphNode>(_nodes.at(index));
}
SubgraphNode const &GraphBuilderTopology::subgraph_node(size_t index) const {
  return std::get<SubgraphNode>(_nodes.at(index));
}
bool GraphBuilderTopology::is_subgraph_node(size_t index) const {
  return std::holds_alternative<SubgraphNode>(_nodes.at(index));
}
NodePorts &GraphBuilderTopology::ports(size_t index) {
  return std::visit([](auto &node) -> NodePorts & { return node.ports; }, _nodes.at(index));
}
NodePorts const &GraphBuilderTopology::ports(size_t index) const {
  return std::visit([](auto const &node) -> NodePorts const & { return node.ports; }, _nodes.at(index));
}
NodeLifetime &GraphBuilderTopology::lifetime(size_t index) {
  return std::visit([](auto &node) -> NodeLifetime & { return node.lifetime; }, _nodes.at(index));
}
NodeLifetime const &GraphBuilderTopology::lifetime(size_t index) const {
  return std::visit([](auto const &node) -> NodeLifetime const & { return node.lifetime; }, _nodes.at(index));
}
NodeTypeIdentity const &GraphBuilderTopology::type_identity(size_t index) const {
  return std::visit([](auto const &node) -> NodeTypeIdentity const & { return node.type_identity; }, _nodes.at(index));
}
size_t GraphBuilderTopology::append_node(ConcreteNode node) {
  _nodes.emplace_back(std::move(node));
  return _nodes.size() - 1;
}
size_t GraphBuilderTopology::append_node(SubgraphNode node) {
  _nodes.push_back(std::move(node));
  return _nodes.size() - 1;
}

void GraphBuilderTopology::apply_ttl(size_t node_index, size_t ttl_samples) {
  lifetime(node_index).ttl_samples = ttl_samples;
  if (!is_subgraph_node(node_index)) {
    return;
  }
  auto &builder_node = subgraph_node(node_index);

  size_t const begin = builder_node.lowered_subgraph.begin;
  size_t const end = begin + builder_node.lowered_subgraph.count;
  for (size_t child_node_index = begin; child_node_index < end;
       ++child_node_index) {
    lifetime(child_node_index).ttl_samples = ttl_samples;
  }
}

void GraphBuilderTopology::add_sample_edge(GraphEdge edge) {
  _edges.emplace(std::move(edge));
}

void GraphBuilderTopology::add_event_edge(GraphEventEdge edge) {
  _event_edges.emplace(std::move(edge));
}

ConcretePortId GraphBuilderTopology::append_scope_sample_input(OutputConfig output) {
  auto const boundary_index = _scope_boundary_ports.size();
  _scope_boundary_ports.push_back(
      ScopeBoundaryPort{.sample_output = std::move(output)});
  return ConcretePortId{GRAPH_ID - 1 - boundary_index, 0};
}

ConcretePortId GraphBuilderTopology::append_scope_event_input(EventOutputConfig output) {
  auto const boundary_index = _scope_boundary_ports.size();
  _scope_boundary_ports.push_back(
      ScopeBoundaryPort{.event_output = std::move(output)});
  return ConcretePortId{GRAPH_ID - 1 - boundary_index, 0};
}

bool GraphBuilderTopology::is_scope_boundary_port(ConcretePortId port) const {
  if (port.node >= GRAPH_ID || port.port != 0) {
    return false;
  }
  auto const boundary_index = GRAPH_ID - 1 - port.node;
  return boundary_index < _scope_boundary_ports.size();
}

EventOutputConfig const&
GraphBuilderTopology::scope_boundary_event_output(ConcretePortId port) const {
  IV_ASSERT(is_scope_boundary_port(port),
            "scope_boundary_event_output requires a boundary port");
  auto const boundary_index = GRAPH_ID - 1 - port.node;
  auto const& boundary = _scope_boundary_ports[boundary_index];
  IV_ASSERT(boundary.event_output.has_value(),
            "scope boundary port is not an event output");
  return *boundary.event_output;
}

size_t GraphBuilderTopology::append_lowered_subgraph_node(
    std::string subgraph_kind, std::vector<InputConfig> input_configs,
    std::vector<OutputConfig> output_configs,
    std::vector<EventInputConfig> event_input_configs,
    std::vector<EventOutputConfig> event_output_configs,
    size_t lowered_subgraph_begin, size_t lowered_subgraph_count,
    std::vector<std::vector<ConcretePortId>> subgraph_input_targets,
    std::vector<ConcretePortId> subgraph_output_sources,
    std::vector<std::vector<ConcretePortId>> subgraph_event_input_targets,
    std::vector<ConcretePortId> subgraph_event_output_sources) {
  std::string type_identity = "lowered-subgraph:" + subgraph_kind;
  return append_node(SubgraphNode{
      .ports =
          NodePorts{
              .sample_inputs = std::move(input_configs),
              .sample_outputs = std::move(output_configs),
              .event_input_configs = std::move(event_input_configs),
              .event_output_configs = std::move(event_output_configs),
          },
      .lifetime = NodeLifetime{.ttl_samples = std::nullopt},
      .lowered_subgraph =
          LoweredSubgraphBinding{
              .begin = lowered_subgraph_begin,
              .count = lowered_subgraph_count,
              .sample_input_targets = std::move(subgraph_input_targets),
              .sample_output_sources = std::move(subgraph_output_sources),
              .event_input_targets = std::move(subgraph_event_input_targets),
              .event_output_sources = std::move(subgraph_event_output_sources),
              .kind = std::move(subgraph_kind),
          },
      .type_identity = NodeTypeIdentity{.value = std::move(type_identity)},
  });
}

size_t GraphBuilderTopology::append_embedded_child(
    GraphBuilderTopology const &child,
    std::span<InputConfig const> child_sample_inputs,
    std::span<OutputConfig const> child_sample_outputs,
    std::span<EventInputConfig const> child_event_inputs,
    std::span<EventOutputConfig const> child_event_outputs,
    size_t child_detach_offset) {
  size_t const subgraph_node_index = node_count();
  size_t const child_node_offset = subgraph_node_index + 1;

  auto remap_child_port = [&](ConcretePortId port) {
    if (port.node == GRAPH_ID) {
      return ConcretePortId{subgraph_node_index, port.port};
    }
    return ConcretePortId{child_node_offset + port.node, port.port};
  };

  append_lowered_subgraph_node(
      {},
      std::vector<InputConfig>(child_sample_inputs.begin(),
                               child_sample_inputs.end()),
      std::vector<OutputConfig>(child_sample_outputs.begin(),
                                child_sample_outputs.end()),
      std::vector<EventInputConfig>(child_event_inputs.begin(),
                                    child_event_inputs.end()),
      std::vector<EventOutputConfig>(child_event_outputs.begin(),
                                     child_event_outputs.end()),
      child_node_offset, child.node_count(),
      std::vector<std::vector<ConcretePortId>>(child_sample_inputs.size()),
      std::vector<ConcretePortId>(child_sample_outputs.size()),
      std::vector<std::vector<ConcretePortId>>(child_event_inputs.size()),
      std::vector<ConcretePortId>(child_event_outputs.size()));

  for (size_t i = 0; i < child.node_count(); ++i) {
    auto copied_node = child._nodes[i];
    std::visit(
        [&](auto &node) {
          using Node = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<Node, ConcreteNode>) {
            auto materialize = std::move(node.materialization.factory);
            node.materialization.factory =
                [materialize = std::move(materialize), child_detach_offset](
                    size_t parent_detach_offset) {
                  return materialize(parent_detach_offset + child_detach_offset);
                };
          } else {
            for (auto &targets : node.lowered_subgraph.sample_input_targets) {
              for (auto &target : targets) target = remap_child_port(target);
            }
            for (auto &source : node.lowered_subgraph.sample_output_sources) source = remap_child_port(source);
            for (auto &targets : node.lowered_subgraph.event_input_targets) {
              for (auto &target : targets) target = remap_child_port(target);
            }
            for (auto &source : node.lowered_subgraph.event_output_sources) source = remap_child_port(source);
          }
        }, copied_node);
    std::visit([&](auto &&node) { append_node(std::move(node)); }, std::move(copied_node));
  }

  child.for_each_sample_edge([&](GraphEdge const &edge) {
    ConcretePortId const source = remap_child_port(edge.source);
    ConcretePortId const target = remap_child_port(edge.target);
    if (source.node == subgraph_node_index && target.node == subgraph_node_index) {
      subgraph_node(subgraph_node_index)
          .lowered_subgraph.sample_output_sources[target.port] = source;
    } else if (source.node == subgraph_node_index) {
      subgraph_node(subgraph_node_index)
          .lowered_subgraph.sample_input_targets[source.port]
          .push_back(target);
    } else if (target.node == subgraph_node_index) {
      subgraph_node(subgraph_node_index)
          .lowered_subgraph.sample_output_sources[target.port] = source;
    } else {
      add_sample_edge(GraphEdge{source, target});
    }
  });

  child.for_each_event_edge([&](GraphEventEdge const &edge) {
    ConcretePortId const source = remap_child_port(edge.source);
    ConcretePortId const target = remap_child_port(edge.target);
    if (source.node == subgraph_node_index && target.node == subgraph_node_index) {
      subgraph_node(subgraph_node_index)
          .lowered_subgraph.event_output_sources[target.port] = source;
    } else if (source.node == subgraph_node_index) {
      subgraph_node(subgraph_node_index)
          .lowered_subgraph.event_input_targets[source.port]
          .push_back(target);
    } else if (target.node == subgraph_node_index) {
      subgraph_node(subgraph_node_index)
          .lowered_subgraph.event_output_sources[target.port] = source;
    } else {
      add_event_edge(GraphEventEdge{source, target, edge.conversion});
    }
  });

  return subgraph_node_index;
}
} // namespace iv
