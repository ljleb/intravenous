#pragma once

#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_refs.h>

#include <cstdint>
#include <string_view>

namespace iv {
class GraphBuilder;
class GraphBuilderTopology;
class GraphBuilderNodeBundles;
class GraphBuilderVirtualNodes;
class GraphBuilderAnnotations {
public:
  void attach_virtual_node(GraphBuilderTopology &, GraphBuilderNodeBundles &,
                           GraphBuilderVirtualNodes &, size_t node_bundle_handle,
                           std::string_view virtual_node_id,
                           SourceInfo const *source_info = nullptr);
  void annotate_node_source_info(GraphBuilderTopology &, GraphBuilderNodeBundles &,
                                 GraphBuilderVirtualNodes &,
                                 GraphBuilderIdentity const &,
                                 GraphBuilder const &owner, NodeRef const &ref,
                                 std::string_view declaration_identity,
                                 std::string_view file_path, uint32_t begin,
                                 uint32_t end);
};
} // namespace iv
