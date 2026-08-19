#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/node.h>

namespace iv {
struct GraphBuilderIdentity;
class GraphBuilderTopology;
class GraphBuilderNodeBundles;
class GraphBuilderConnections;
class GraphBuilderPublicPorts;
class GraphBuilderDetach;
class GraphBuilderVirtualNodes;

struct GraphBuilderRootNodeBuildResult {
  Graph graph;
  GraphBuildMetadata metadata;
};

class GraphBuilderFinalizer {
public:
  static GraphIntrospectionMetadata
  build_metadata(GraphBuilderIdentity const &identity,
                 GraphBuilderTopology const &topology,
                 GraphBuilderNodeBundles const &node_bundles,
                 GraphBuilderVirtualNodes const &virtual_nodes,
                 GraphBuilderConnections const &connections,
                 size_t detach_id_offset);
  static GraphBuilderRootNodeBuildResult
  build_root_node(GraphBuilderIdentity const &identity,
                  GraphBuilderTopology const &topology,
                  GraphBuilderNodeBundles const &node_bundles,
                  GraphBuilderVirtualNodes const &virtual_nodes,
                  GraphBuilderConnections const &connections,
                  GraphBuilderPublicPorts const &public_ports,
                  GraphBuilderDetach const &detach, size_t detach_id_offset);
};
} // namespace iv
