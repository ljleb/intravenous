#pragma once

#include <intravenous/graph/builder/topology.h>
#include <intravenous/graph/builder/node_bundles.hpp>

#include <flat_map>
#include <flat_set>
#include <limits>
#include <optional>
#include <vector>

namespace iv {
class GraphBuilderConnections;
class GraphBuilderPublicPorts;
class GraphBuilderDetach;
class GraphBuilderVirtualNodes;

struct LoweredNodeBundleProjection {
  std::vector<std::vector<TopologyPortId>> sample_inputs{};
  std::vector<std::vector<TopologyPortId>> sample_outputs{};
  std::vector<std::vector<TopologyPortId>> event_inputs{};
  std::vector<std::vector<TopologyPortId>> event_outputs{};
  std::optional<size_t> topology_node{};
};

struct DetachedSamplePortInfo {
  size_t detach_id = 0;
  TopologyPortId original_source{};
  size_t writer_node = std::numeric_limits<size_t>::max();
  TopologyPortId reader_output{};
  size_t loop_extra_latency = 1;
};

struct LoweredBuilderGraph {
  LoweredTopology topology{};
  std::vector<LoweredNodeBundleProjection> bundle_projections{};
  std::vector<std::optional<NodeBundleHandle>> bundle_by_lowered_node{};
  std::flat_map<TopologyPortId, TopologyPortId>
      subgraph_input_of_boundary_source{};
  std::flat_map<TopologyPortId, TopologyPortId>
      subgraph_event_input_of_boundary_source{};
  std::flat_map<TopologyPortId, DetachedSamplePortInfo>
      detached_info_by_source{};
  std::flat_set<TopologyPortId> detached_reader_outputs{};
};

class GraphBuilderLowering {
public:
  static constexpr LoweredBuilderGraph lower(
      GraphBuilderNodeBundles const&, GraphBuilderConnections const&,
      GraphBuilderPublicPorts const&, GraphBuilderVirtualNodes const&,
      GraphBuilderDetach const&,
      bool execution_root = false);
};
} // namespace iv
