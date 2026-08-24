#pragma once

#include <intravenous/graph/builder/topology_port.h>
#include <intravenous/graph/generated_node_spec.h>
#include <intravenous/graph/reflected_node.h>
#include <intravenous/graph/types.h>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace iv {
struct NodeLifetime {
  std::optional<size_t> ttl_samples{};
};

struct LoweredSubgraphBinding {
  size_t begin = 0;
  size_t count = 0;
  std::vector<std::vector<TopologyPortId>> sample_input_targets{};
  std::vector<TopologyPortId> sample_output_sources{};
  std::vector<std::vector<TopologyPortId>> event_input_targets{};
  std::vector<TopologyPortId> event_output_sources{};
  std::string kind{};

  bool active() const;
};

struct NodeSourceAnnotations {
  std::vector<SourceInfo> infos{};
};

struct NodeTypeIdentity {
  std::string value{};
};

// Lowered execution-topology IR produced from semantic NodeBundles.
struct ConcreteNode {
  NodePorts ports{};
  ReflectedNodeOperations operations{};
  NodeLifetime lifetime{};
  NodeTypeIdentity type_identity{};
  size_t internal_latency_samples = 0;
  size_t maximum_block_size = MAX_BLOCK_SIZE;
  std::optional<size_t> default_ttl_samples{};
  bool block_skippable = false;
  GeneratedNodeSpec generated_node{};

  std::vector<InputConfig> const &inputs() const;
  std::vector<OutputConfig> const &outputs() const;
  std::vector<EventInputConfig> const &event_inputs() const;
  std::vector<EventOutputConfig> const &event_outputs() const;
};

struct SubgraphNode {
  NodePorts ports{};
  NodeLifetime lifetime{};
  LoweredSubgraphBinding lowered_subgraph{};
  NodeTypeIdentity type_identity{};

  std::vector<InputConfig> const &inputs() const;
  std::vector<OutputConfig> const &outputs() const;
  std::vector<EventInputConfig> const &event_inputs() const;
  std::vector<EventOutputConfig> const &event_outputs() const;
};

using StoredNode = std::variant<ConcreteNode, SubgraphNode>;

} // namespace iv
