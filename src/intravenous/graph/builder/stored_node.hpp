#pragma once

#include <intravenous/graph/builder/topology_port.h>
#include <intravenous/graph/generated_node_spec.hpp>
#include <intravenous/graph/reflected_node.hpp>
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

  constexpr bool active() const {
    return count != 0
        || !sample_input_targets.empty()
        || !sample_output_sources.empty()
        || !event_input_targets.empty()
        || !event_output_sources.empty()
        || !kind.empty();
  }
};

struct NodeSourceAnnotations {
  std::vector<SourceInfo> infos{};
};

struct NodeTypeIdentity {
  std::string value{};
};

enum class DeferredDetachNodeKind {
  writer,
  reader,
};

// Child builders use local detach IDs. Keep this closed pair of built-ins as
// semantic records until GraphBuilder::finish(), when every child offset has
// been resolved and the final executable node values can be reflected once.
struct DeferredDetachNode {
  DeferredDetachNodeKind kind = DeferredDetachNodeKind::writer;
  size_t id = 0;
  size_t loop_extra_latency = 1;
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
  // Static data attached to an authored node survives semantic lowering so
  // later compiler passes can materialize it without a ticking producer.
  std::optional<Sample> static_sample_value{};
  std::optional<DeferredDetachNode> deferred_detach{};
  GeneratedNodeSpec generated_node{};

  constexpr std::vector<InputConfig> const& inputs() const {
    return ports.inputs();
  }
  constexpr std::vector<OutputConfig> const& outputs() const {
    return ports.outputs();
  }
  constexpr std::vector<EventInputConfig> const& event_inputs() const {
    return ports.event_inputs();
  }
  constexpr std::vector<EventOutputConfig> const& event_outputs() const {
    return ports.event_outputs();
  }
};

struct SubgraphNode {
  NodePorts ports{};
  NodeLifetime lifetime{};
  LoweredSubgraphBinding lowered_subgraph{};
  NodeTypeIdentity type_identity{};

  constexpr std::vector<InputConfig> const& inputs() const {
    return ports.inputs();
  }
  constexpr std::vector<OutputConfig> const& outputs() const {
    return ports.outputs();
  }
  constexpr std::vector<EventInputConfig> const& event_inputs() const {
    return ports.event_inputs();
  }
  constexpr std::vector<EventOutputConfig> const& event_outputs() const {
    return ports.event_outputs();
  }
};

using StoredNode = std::variant<ConcreteNode, SubgraphNode>;

} // namespace iv
