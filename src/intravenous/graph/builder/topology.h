#pragma once

#include <intravenous/graph/builder/stored_node.h>
#include <intravenous/graph/builder/topology_port.h>

#include <intravenous/graph/compiler.h>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace iv {
struct TopologyEdge {
  TopologyPortId source{};
  TopologyPortId target{};
  bool operator==(TopologyEdge const&) const = default;
};

struct TopologyEventEdge {
  TopologyPortId source{};
  TopologyPortId target{};
  EventConversionPlan conversion{};
  bool operator==(TopologyEventEdge const& rhs) const {
    return source == rhs.source && target == rhs.target;
  }
};

// Compiler IR only. GraphBuilder never owns one: lowering creates a fresh
// topology from semantic bundles/connections for each build/metadata pass.
class LoweredTopology {
public:
  size_t node_count() const;
  ConcreteNode& concrete_node(size_t);
  ConcreteNode const& concrete_node(size_t) const;
  SubgraphNode& subgraph_node(size_t);
  SubgraphNode const& subgraph_node(size_t) const;
  bool is_subgraph_node(size_t) const;
  size_t append_node(ConcreteNode);
  size_t append_node(SubgraphNode);
  size_t append_lowered_subgraph_node(
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

  // Allocate lowering-only sentinel sources for non-root semantic boundaries.
  TopologyPortId append_scope_sample_input(OutputConfig);
  TopologyPortId append_scope_event_input(EventOutputConfig);

  void add_sample_edge(TopologyEdge);
  void add_event_edge(TopologyEventEdge);

  template<class Fn>
  void for_each_sample_edge(Fn&& fn) const {
    for (auto const& edge : _edges) fn(edge);
  }
  template<class Fn>
  void for_each_event_edge(Fn&& fn) const {
    for (auto const& edge : _event_edges) fn(edge);
  }

private:
  struct TopologyEdgeHash {
    size_t operator()(TopologyEdge const& edge) const noexcept {
      return std::hash<TopologyPortId>{}(edge.source) ^
             (std::hash<TopologyPortId>{}(edge.target) << 1);
    }
  };
  struct TopologyEventEdgeHash {
    size_t operator()(TopologyEventEdge const& edge) const noexcept {
      return std::hash<TopologyPortId>{}(edge.source) ^
             (std::hash<TopologyPortId>{}(edge.target) << 1);
    }
  };

  std::vector<StoredNode> _nodes{};
  std::unordered_set<TopologyEdge, TopologyEdgeHash> _edges{};
  std::unordered_set<TopologyEventEdge, TopologyEventEdgeHash> _event_edges{};
  size_t _scope_boundary_port_count = 0;
};
} // namespace iv
