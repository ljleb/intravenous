#pragma once
#include <intravenous/graph/build_types.h>
#include <intravenous/graph/node.h>
namespace iv {
struct GraphBuilderIdentity; struct LoweredBuilderGraph;
class GraphBuilderNodeBundles; class GraphBuilderConnections; class GraphBuilderPublicPorts; class GraphBuilderVirtualNodes;
struct GraphBuilderRootNodeBuildResult { Graph graph; GraphBuildMetadata metadata; };
class GraphBuilderFinalizer {
public:
  static constexpr GraphIntrospectionMetadata build_metadata(
      GraphBuilderIdentity const&, LoweredBuilderGraph const&,
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&,
      GraphBuilderConnections const&, size_t detach_id_offset);
  static consteval GraphBuilderRootNodeBuildResult build_root_node(
      GraphBuilderIdentity const&, LoweredBuilderGraph const&,
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&,
      GraphBuilderPublicPorts const&, size_t detach_id_offset,
      bool execution_root = false);
};
} // namespace iv
