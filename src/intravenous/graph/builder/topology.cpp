#include <intravenous/graph/builder/topology.h>

#include <utility>

namespace iv {
size_t LoweredTopology::node_count() const { return _nodes.size(); }
ConcreteNode& LoweredTopology::concrete_node(size_t index) {
  return std::get<ConcreteNode>(_nodes.at(index));
}
ConcreteNode const& LoweredTopology::concrete_node(size_t index) const {
  return std::get<ConcreteNode>(_nodes.at(index));
}
SubgraphNode& LoweredTopology::subgraph_node(size_t index) {
  return std::get<SubgraphNode>(_nodes.at(index));
}
SubgraphNode const& LoweredTopology::subgraph_node(size_t index) const {
  return std::get<SubgraphNode>(_nodes.at(index));
}
bool LoweredTopology::is_subgraph_node(size_t index) const {
  return std::holds_alternative<SubgraphNode>(_nodes.at(index));
}
size_t LoweredTopology::append_node(ConcreteNode node) {
  _nodes.emplace_back(std::move(node));
  return _nodes.size() - 1;
}
size_t LoweredTopology::append_node(SubgraphNode node) {
  _nodes.emplace_back(std::move(node));
  return _nodes.size() - 1;
}

void LoweredTopology::add_sample_edge(TopologyEdge edge) {
  _edges.emplace(std::move(edge));
}
void LoweredTopology::add_event_edge(TopologyEventEdge edge) {
  _event_edges.emplace(std::move(edge));
}

void LoweredTopology::replace_sample_source(TopologyPortId from, TopologyPortId to) {
  std::unordered_set<TopologyEdge, TopologyEdgeHash> replaced;
  replaced.reserve(_edges.size());
  for (auto edge : _edges) {
    if (edge.source == from) edge.source = to;
    replaced.emplace(std::move(edge));
  }
  _edges = std::move(replaced);
}

void LoweredTopology::replace_sample_target(TopologyPortId from, TopologyPortId to) {
  std::unordered_set<TopologyEdge, TopologyEdgeHash> replaced;
  replaced.reserve(_edges.size());
  for (auto edge : _edges) {
    if (edge.target == from) edge.target = to;
    replaced.emplace(std::move(edge));
  }
  _edges = std::move(replaced);
}

void LoweredTopology::replace_event_source(TopologyPortId from, TopologyPortId to) {
  std::unordered_set<TopologyEventEdge, TopologyEventEdgeHash> replaced;
  replaced.reserve(_event_edges.size());
  for (auto edge : _event_edges) {
    if (edge.source == from) edge.source = to;
    replaced.emplace(std::move(edge));
  }
  _event_edges = std::move(replaced);
}

void LoweredTopology::replace_event_target(TopologyPortId from, TopologyPortId to) {
  std::unordered_set<TopologyEventEdge, TopologyEventEdgeHash> replaced;
  replaced.reserve(_event_edges.size());
  for (auto edge : _event_edges) {
    if (edge.target == from) edge.target = to;
    replaced.emplace(std::move(edge));
  }
  _event_edges = std::move(replaced);
}

TopologyPortId LoweredTopology::append_scope_sample_input(OutputConfig) {
  auto const ordinal = _scope_boundary_port_count++;
  return {GRAPH_ID - 1 - ordinal, 0};
}
TopologyPortId LoweredTopology::append_scope_event_input(EventOutputConfig) {
  auto const ordinal = _scope_boundary_port_count++;
  return {GRAPH_ID - 1 - ordinal, 0};
}

size_t LoweredTopology::append_lowered_subgraph_node(
    std::string subgraph_kind,
    std::vector<InputConfig> input_configs,
    std::vector<OutputConfig> output_configs,
    std::vector<EventInputConfig> event_input_configs,
    std::vector<EventOutputConfig> event_output_configs,
    size_t lowered_subgraph_begin,
    size_t lowered_subgraph_count,
    std::vector<std::vector<TopologyPortId>> subgraph_input_targets,
    std::vector<TopologyPortId> subgraph_output_sources,
    std::vector<std::vector<TopologyPortId>> subgraph_event_input_targets,
    std::vector<TopologyPortId> subgraph_event_output_sources) {
  std::string type_identity = "lowered-subgraph:" + subgraph_kind;
  return append_node(SubgraphNode{
      .ports = NodePorts{
          .sample_inputs = std::move(input_configs),
          .sample_outputs = std::move(output_configs),
          .event_input_configs = std::move(event_input_configs),
          .event_output_configs = std::move(event_output_configs),
      },
      .lifetime = NodeLifetime{.ttl_samples = std::nullopt},
      .lowered_subgraph = LoweredSubgraphBinding{
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
} // namespace iv
