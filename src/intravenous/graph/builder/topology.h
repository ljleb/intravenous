#pragma once

#include <intravenous/graph/builder/stored_node.hpp>
#include <intravenous/graph/builder/topology_port.h>

#include <intravenous/graph/compiler.h>

#include <algorithm>
#include <flat_set>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace iv {
struct TopologyEdge {
  TopologyPortId source{};
  TopologyPortId target{};
  auto operator<=>(TopologyEdge const&) const = default;
};

struct TopologyEventEdge {
  TopologyPortId source{};
  TopologyPortId target{};
  EventConversionPlan conversion{};
  bool operator==(TopologyEventEdge const& rhs) const {
    return source == rhs.source && target == rhs.target;
  }
  auto operator<=>(TopologyEventEdge const& rhs) const {
    if (auto ordering = source <=> rhs.source; ordering != 0)
      return ordering;
    return target <=> rhs.target;
  }
};

// Compiler IR only. GraphBuilder never owns one: lowering creates a fresh
// topology from semantic bundles/connections for each build/metadata pass.
class LoweredTopology {
public:
  constexpr size_t node_count() const;
  constexpr ConcreteNode& concrete_node(size_t);
  constexpr ConcreteNode const& concrete_node(size_t) const;
  constexpr SubgraphNode& subgraph_node(size_t);
  constexpr SubgraphNode const& subgraph_node(size_t) const;
  constexpr bool is_subgraph_node(size_t) const;
  constexpr size_t append_node(ConcreteNode);
  constexpr size_t append_node(SubgraphNode);
  constexpr size_t append_lowered_subgraph_node(
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
      std::vector<TopologyPortId> subgraph_event_output_sources);

  constexpr TopologyPortId append_scope_sample_input(OutputConfig);
  constexpr TopologyPortId append_scope_event_input(EventOutputConfig);

  constexpr void add_sample_edge(TopologyEdge);
  constexpr void add_event_edge(TopologyEventEdge);
  constexpr void replace_sample_source(TopologyPortId from, TopologyPortId to);
  constexpr void replace_sample_target(TopologyPortId from, TopologyPortId to);
  constexpr void replace_event_source(TopologyPortId from, TopologyPortId to);
  constexpr void replace_event_target(TopologyPortId from, TopologyPortId to);
  constexpr void normalize_edges();

  template<class Fn>
  constexpr void for_each_sample_edge(Fn&& fn) {
    normalize_sample_edges();
    for (auto const& edge : _edges) fn(edge);
  }
  template<class Fn>
  constexpr void for_each_sample_edge(Fn&& fn) const {
    for (auto const& edge : _edges) fn(edge);
  }
  template<class Fn>
  constexpr void for_each_event_edge(Fn&& fn) {
    normalize_event_edges();
    for (auto const& edge : _event_edges) fn(edge);
  }
  template<class Fn>
  constexpr void for_each_event_edge(Fn&& fn) const {
    for (auto const& edge : _event_edges) fn(edge);
  }

private:
  constexpr void normalize_sample_edges();
  constexpr void normalize_event_edges();

  std::vector<StoredNode> _nodes{};
  std::flat_set<TopologyEdge> _edges{};
  std::flat_set<TopologyEventEdge> _event_edges{};
  std::vector<TopologyEdge> _pending_edges{};
  std::vector<TopologyEventEdge> _pending_event_edges{};
  size_t _scope_boundary_port_count = 0;
};

constexpr size_t LoweredTopology::node_count() const { return _nodes.size(); }
constexpr ConcreteNode& LoweredTopology::concrete_node(size_t index) {
  return std::get<ConcreteNode>(_nodes.at(index));
}
constexpr ConcreteNode const& LoweredTopology::concrete_node(size_t index) const {
  return std::get<ConcreteNode>(_nodes.at(index));
}
constexpr SubgraphNode& LoweredTopology::subgraph_node(size_t index) {
  return std::get<SubgraphNode>(_nodes.at(index));
}
constexpr SubgraphNode const& LoweredTopology::subgraph_node(size_t index) const {
  return std::get<SubgraphNode>(_nodes.at(index));
}
constexpr bool LoweredTopology::is_subgraph_node(size_t index) const {
  return std::holds_alternative<SubgraphNode>(_nodes.at(index));
}
constexpr size_t LoweredTopology::append_node(ConcreteNode node) {
  _nodes.emplace_back(std::move(node));
  return _nodes.size() - 1;
}
constexpr size_t LoweredTopology::append_node(SubgraphNode node) {
  _nodes.emplace_back(std::move(node));
  return _nodes.size() - 1;
}

constexpr void LoweredTopology::add_sample_edge(TopologyEdge edge) {
  _pending_edges.push_back(std::move(edge));
}
constexpr void LoweredTopology::add_event_edge(TopologyEventEdge edge) {
  _pending_event_edges.push_back(std::move(edge));
}

constexpr void LoweredTopology::normalize_sample_edges() {
  if (_pending_edges.empty()) return;
  _edges.insert_range(_pending_edges);
  _pending_edges.clear();
}

constexpr void LoweredTopology::normalize_event_edges() {
  if (_pending_event_edges.empty()) return;
  _event_edges.insert_range(_pending_event_edges);
  _pending_event_edges.clear();
}

constexpr void LoweredTopology::normalize_edges() {
  normalize_sample_edges();
  normalize_event_edges();
}

constexpr void LoweredTopology::replace_sample_source(
    TopologyPortId from, TopologyPortId to) {
  normalize_sample_edges();
  auto replaced = std::move(_edges).extract();
  for (auto& edge : replaced) {
    if (edge.source == from) edge.source = to;
  }
  std::ranges::sort(replaced);
  replaced.erase(std::ranges::unique(replaced).begin(), replaced.end());
  _edges.replace(std::move(replaced));
}

constexpr void LoweredTopology::replace_sample_target(
    TopologyPortId from, TopologyPortId to) {
  normalize_sample_edges();
  auto replaced = std::move(_edges).extract();
  for (auto& edge : replaced) {
    if (edge.target == from) edge.target = to;
  }
  std::ranges::sort(replaced);
  replaced.erase(std::ranges::unique(replaced).begin(), replaced.end());
  _edges.replace(std::move(replaced));
}

constexpr void LoweredTopology::replace_event_source(
    TopologyPortId from, TopologyPortId to) {
  normalize_event_edges();
  auto replaced = std::move(_event_edges).extract();
  for (auto& edge : replaced) {
    if (edge.source == from) edge.source = to;
  }
  std::ranges::sort(replaced);
  replaced.erase(std::ranges::unique(replaced).begin(), replaced.end());
  _event_edges.replace(std::move(replaced));
}

constexpr void LoweredTopology::replace_event_target(
    TopologyPortId from, TopologyPortId to) {
  normalize_event_edges();
  auto replaced = std::move(_event_edges).extract();
  for (auto& edge : replaced) {
    if (edge.target == from) edge.target = to;
  }
  std::ranges::sort(replaced);
  replaced.erase(std::ranges::unique(replaced).begin(), replaced.end());
  _event_edges.replace(std::move(replaced));
}

constexpr TopologyPortId LoweredTopology::append_scope_sample_input(
    OutputConfig) {
  auto const ordinal = _scope_boundary_port_count++;
  return {GRAPH_ID - 1 - ordinal, 0};
}

constexpr TopologyPortId LoweredTopology::append_scope_event_input(
    EventOutputConfig) {
  auto const ordinal = _scope_boundary_port_count++;
  return {GRAPH_ID - 1 - ordinal, 0};
}

constexpr size_t LoweredTopology::append_lowered_subgraph_node(
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
