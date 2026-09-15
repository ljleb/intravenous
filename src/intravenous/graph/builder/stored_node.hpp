#pragma once

#include <intravenous/graph/builder/topology_port.h>
#include <intravenous/graph/generated_node_spec.hpp>
#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/graph/types.h>
#include <intravenous/node/registered_type_identity.h>

#include <memory>
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

// Configured concrete-node data stays owned by GraphBuilderNodeBundles throughout
// topology lowering. The workspace carries this lightweight handle instead of
// copying ports, strings, callbacks, and static values into another graph
// representation.
struct ConfiguredConcreteNodeRef {
  size_t node_bundle_handle = 0;
};

// Lowered execution-topology IR produced from semantic NodeBundles.
struct ConcreteNode {
  NodePorts ports{};
  ReflectedNodeOperations operations{};
  std::shared_ptr<void const> node_storage{};
  std::shared_ptr<NodeStateStructure const> state_structure_storage{};
  NodeConfigRelocations config_relocations{};
  NodeCodeKey code_key{};
  std::optional<RegisteredNodeTypeIdentity> registered_node_type_identity{};
  size_t node_size = 0;
  size_t node_alignment = 1;
  NodeLifetime lifetime{};
  NodeTypeIdentity type_identity{};
  // Reflection supplies a static type spelling. Keep that stable view separate
  // from the owned identity used for graph-generated names and metadata.
  std::string_view reflected_type_name{};
  size_t internal_latency_samples = 0;
  size_t maximum_block_size = MAX_BLOCK_SIZE;
  std::optional<size_t> default_ttl_samples{};
  bool block_skippable = false;
  // Static data attached to an configured node survives semantic lowering so
  // later compiler passes can materialize it without a ticking producer.
  std::optional<Sample> static_sample_value{};
  std::optional<DeferredDetachNode> deferred_detach{};
  GeneratedNodeSpec generated_node{};

  std::vector<InputConfig> inputs() const {
    std::vector<InputConfig> result;
    result.reserve(ports.sample_inputs.size() + ports.event_input_configs.size());
    for (SampleInputConfig const& input : ports.sample_inputs) {
      result.emplace_back(input.name, SampleInputProperties{
          .channel_layout = input.channel_layout, .history = input.history, .default_value = input.default_value,
          .min = input.min, .max = input.max});
    }
    for (EventInputConfig const& input : ports.event_input_configs)
      result.emplace_back(input.name, EventInputProperties{.type = input.type});
    return result;
  }
  std::vector<OutputConfig> outputs() const {
    std::vector<OutputConfig> result;
    result.reserve(ports.sample_outputs.size() + ports.event_output_configs.size());
    for (SampleOutputConfig const& output : ports.sample_outputs) {
      result.emplace_back(output.name, SampleOutputProperties{
          .channel_layout = output.channel_layout, .latency = output.latency,
          .history = output.history});
    }
    for (EventOutputConfig const& output : ports.event_output_configs)
      result.emplace_back(output.name, EventOutputProperties{.type = output.type});
    return result;
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

  std::vector<InputConfig> inputs() const {
    std::vector<InputConfig> result;
    result.reserve(ports.sample_inputs.size() + ports.event_input_configs.size());
    for (SampleInputConfig const& input : ports.sample_inputs) {
      result.emplace_back(input.name, SampleInputProperties{
          .channel_layout = input.channel_layout, .history = input.history, .default_value = input.default_value,
          .min = input.min, .max = input.max});
    }
    for (EventInputConfig const& input : ports.event_input_configs)
      result.emplace_back(input.name, EventInputProperties{.type = input.type});
    return result;
  }
  std::vector<OutputConfig> outputs() const {
    std::vector<OutputConfig> result;
    result.reserve(ports.sample_outputs.size() + ports.event_output_configs.size());
    for (SampleOutputConfig const& output : ports.sample_outputs) {
      result.emplace_back(output.name, SampleOutputProperties{
          .channel_layout = output.channel_layout, .latency = output.latency,
          .history = output.history});
    }
    for (EventOutputConfig const& output : ports.event_output_configs)
      result.emplace_back(output.name, EventOutputProperties{.type = output.type});
    return result;
  }
  constexpr std::vector<EventInputConfig> const& event_inputs() const {
    return ports.event_inputs();
  }
  constexpr std::vector<EventOutputConfig> const& event_outputs() const {
    return ports.event_outputs();
  }
};

using StoredNode = std::variant<ConfiguredConcreteNodeRef, ConcreteNode,
                                SubgraphNode>;

} // namespace iv
