#pragma once

#include <cstddef>

namespace iv {
class GraphBuilderTopology;
class GraphBuilderNodeBundles;
class GraphBuilderConnections;
class GraphBuilderPublicPorts;
class GraphBuilderDetach;
class GraphBuilderVirtualNodes;

class GraphBuilderChildEmbedder {
public:
  static size_t embed(GraphBuilderTopology &parent_topology,
                      GraphBuilderNodeBundles &parent_node_bundles,
                      GraphBuilderConnections &parent_connections,
                      GraphBuilderDetach &parent_detach,
                      GraphBuilderVirtualNodes &parent_virtual_nodes,
                      GraphBuilderPublicPorts const &child_public_ports,
                      GraphBuilderTopology const &child_topology,
                      GraphBuilderNodeBundles const &child_node_bundles,
                      GraphBuilderConnections const &child_connections,
                      GraphBuilderDetach const &child_detach,
                      GraphBuilderVirtualNodes const &child_virtual_nodes);
};
} // namespace iv
