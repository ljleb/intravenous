#pragma once
#include <cstddef>
namespace iv {
class GraphBuilderNodeBundles; class GraphBuilderConnections; class GraphBuilderPublicPorts;
class GraphBuilderDetach; class GraphBuilderVirtualNodes;
class GraphBuilderChildEmbedder {
public:
  static size_t embed(GraphBuilderNodeBundles&, GraphBuilderConnections&,
      GraphBuilderDetach&, GraphBuilderVirtualNodes&,
      GraphBuilderPublicPorts const&, GraphBuilderNodeBundles const&,
      GraphBuilderConnections const&, GraphBuilderDetach const&,
      GraphBuilderVirtualNodes const&);
};
} // namespace iv
