#pragma once

#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/graph/source_info.h>
#include <intravenous/node/registered_type_identity.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iv {
struct NodeLifetime {
  std::optional<std::size_t> ttl_samples{};
};

struct NodeSourceAnnotations {
  std::vector<SourceInfo> infos{};
};

struct NodeTypeIdentity {
  std::string value{};
};

struct ConcreteNode {
  NodePorts ports{};
  ReflectedNodeOperations operations{};
  std::shared_ptr<void const> node_storage{};
  std::shared_ptr<NodeStateStructures const> state_structures_storage{};
  NodeConfigRelocations config_relocations{};
  NodeCodeKey code_key{};
  std::optional<RegisteredNodeTypeIdentity> registered_node_type_identity{};
  std::size_t node_size = 0;
  std::size_t node_alignment = 1;
  NodeLifetime lifetime{};
  NodeTypeIdentity type_identity{};
  // Reflection supplies a static type spelling. Keep that stable view separate
  // from the owned identity used for graph-generated names and metadata.
  std::string_view reflected_type_name{};
  std::size_t internal_latency_samples = 0;
  std::size_t maximum_block_size = MAX_BLOCK_SIZE;
  std::optional<std::size_t> default_ttl_samples{};
  bool block_skippable = false;
  // Static data attached to a configured node survives graph composition so
  // later compiler passes can materialize it without a ticking producer.
  std::optional<Sample> static_sample_value{};

  constexpr std::vector<InputConfig> const& inputs() const {
    return ports.inputs();
  }
  constexpr std::vector<OutputConfig> const& outputs() const {
    return ports.outputs();
  }
};

} // namespace iv
