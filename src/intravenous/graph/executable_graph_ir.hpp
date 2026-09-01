#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/reflected_node.hpp>

#include <flat_map>
#include <flat_set>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iv {

// The complete static topology at the authored-to-executable bottleneck.
// It is the only mutable graph-shaped data shared with GraphCompiler.
struct ExecutableGraphData {
  std::vector<ReflectedNodeDescription> nodes;
  // Compiler-generated node objects are host-owned. Descriptions retain only
  // opaque callbacks and node_data pointers, so this storage must travel with
  // the final runtime graph plan.
  std::vector<std::shared_ptr<void const>> generated_node_storage;
  std::vector<std::optional<size_t>> explicit_ttl_samples;
  std::vector<std::string> node_ids;
  std::vector<std::vector<std::string>> node_virtual_ids;
  std::vector<std::vector<SourceInfo>> node_source_infos;
  std::vector<size_t> node_construction_order;
  std::vector<std::string> node_kinds;
  std::vector<std::string> node_type_identities;
  std::flat_set<GraphEdge> edges;
  std::flat_set<GraphEventEdge> event_edges;
  std::flat_map<ConcretePortId, DetachedInfo> detached_info_by_source;
  std::flat_set<ConcretePortId> detached_reader_outputs;
};

// A closed executable graph. All semantic node/edge synthesis and authored
// provenance transfer is complete before this value is constructed.
struct ExecutableGraphIR {
  std::string graph_id{};
  ExecutableGraphData graph{};
  std::vector<LoweredSubgraphSpec> scopes{};
  std::vector<InputConfig> public_inputs{};
  std::vector<OutputConfig> public_outputs{};
  std::vector<EventInputConfig> public_event_inputs{};
  std::vector<EventOutputConfig> public_event_outputs{};
  // Lowering has already grouped virtual metadata by backing runtime node.
  // Node IDs are stable across compiler scheduling, so compiler forwards this
  // finished provenance instead of reconstructing it from the IR.
  std::flat_map<std::string, std::vector<std::string>>
      virtual_node_ids_by_backing_node_id{};
  GraphIntrospectionMetadata introspection{};
};

} // namespace iv
