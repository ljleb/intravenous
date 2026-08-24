#pragma once

#include <intravenous/graph/builder/names.h>

#include <algorithm>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <flat_map>
#include <utility>
#include <vector>

namespace iv {
class GraphBuilderNodeBundles;
class GraphBuilderVirtualNodes;
class GraphBuilderConnections;

namespace details {
struct VirtualConcretePortInfo {
  std::string name;
  std::string type;
  bool connected = false;
  size_t history = 0;
  size_t latency = 0;
  Sample default_value = 0.0f;
  std::optional<Sample> min{};
  std::optional<Sample> max{};
};

struct VirtualConcreteNode {
  std::string id;
  std::string kind;
  std::string type_identity;
  size_t construction_order = 0;
  std::vector<SourceInfo> source_infos;
  std::vector<VirtualConcretePortInfo> sample_inputs;
  std::vector<VirtualConcretePortInfo> sample_outputs;
  std::vector<VirtualConcretePortInfo> event_inputs;
  std::vector<VirtualConcretePortInfo> event_outputs;
};

struct MetadataGraphNode {
  std::vector<InputConfig> input_configs;
  std::vector<OutputConfig> output_configs;
  std::vector<EventInputConfig> event_input_configs;
  std::vector<EventOutputConfig> event_output_configs;

  std::vector<InputConfig> const& inputs() const { return input_configs; }
  std::vector<OutputConfig> const& outputs() const { return output_configs; }
  std::vector<EventInputConfig> const& event_inputs() const {
    return event_input_configs;
  }
  std::vector<EventOutputConfig> const& event_outputs() const {
    return event_output_configs;
  }

  void tick(TickSampleContext<MetadataGraphNode> const&) const {}
};

VirtualPortConnectivity
aggregate_connectivity(std::span<VirtualConcretePortInfo const> ports);
void sort_and_deduplicate_spans(std::vector<SourceSpan>& spans);
std::vector<SourceSpan>
source_spans_for(std::span<VirtualConcreteNode const* const> nodes);

inline std::vector<IntrospectionPortInfo> aggregate_ports(
    std::span<VirtualConcreteNode const* const> nodes,
    auto VirtualConcreteNode::* member) {
  if (nodes.empty()) return {};

  auto const& first_ports = nodes.front()->*member;
  std::vector<IntrospectionPortInfo> virtual_ports;
  virtual_ports.reserve(first_ports.size());
  for (size_t i = 0; i < first_ports.size(); ++i) {
    std::vector<VirtualConcretePortInfo> concrete_ports;
    concrete_ports.reserve(nodes.size());
    for (auto const* node : nodes) {
      concrete_ports.push_back((node->*member)[i]);
    }

    virtual_ports.push_back(IntrospectionPortInfo{
        .name = first_ports[i].name,
        .type = first_ports[i].type,
        .connectivity = aggregate_connectivity(concrete_ports),
        .ordinal = i,
        .default_value = first_ports[i].default_value,
        .min = first_ports[i].min,
        .max = first_ports[i].max,
        .history = first_ports[i].history,
        .latency = first_ports[i].latency,
    });
  }
  return virtual_ports;
}

using VirtualMetadataBuildResult = std::pair<
    std::vector<IntrospectionVirtualNode>,
    std::flat_map<std::string, std::vector<std::string>>>;

constexpr VirtualMetadataBuildResult build_virtual_metadata(
    PreparedGraph const& g,
    std::span<LoweredSubgraphSpec const> lowered_scopes);

void apply_virtual_port_metadata(
    GraphIntrospectionMetadata&, GraphBuilderNodeBundles const&,
    GraphBuilderVirtualNodes const&, GraphBuilderConnections const&);
} // namespace details
} // namespace iv
