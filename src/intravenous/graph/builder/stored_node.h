#pragma once

#include <intravenous/graph/node.h>

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace iv {
struct NodePorts {
  std::vector<InputConfig> sample_inputs{};
  std::vector<OutputConfig> sample_outputs{};
  std::vector<EventInputConfig> event_input_configs{};
  std::vector<EventOutputConfig> event_output_configs{};

  std::vector<InputConfig> const &inputs() const;
  std::vector<OutputConfig> const &outputs() const;
  std::vector<EventInputConfig> const &event_inputs() const;
  std::vector<EventOutputConfig> const &event_outputs() const;
};

struct NodeMaterialization {
  std::function<TypeErasedNode(size_t)> factory{};

  bool is_placeholder() const;
  TypeErasedNode make(size_t detach_id_offset) const;
};

struct NodeLifetime {
  std::optional<size_t> ttl_samples{};
};

struct LoweredSubgraphBinding {
  size_t begin = 0;
  size_t count = 0;
  std::vector<std::vector<PortId>> sample_input_targets{};
  std::vector<PortId> sample_output_sources{};
  std::vector<std::vector<PortId>> event_input_targets{};
  std::vector<PortId> event_output_sources{};
  std::string kind{};

  bool active() const;
};

struct NodeSourceAnnotations {
  std::vector<SourceInfo> infos{};
};

struct NodeTypeIdentity {
  std::string value{};
};

struct ConcreteNode {
  NodePorts ports{};
  NodeMaterialization materialization{};
  NodeLifetime lifetime{};
  NodeTypeIdentity type_identity{};

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
