#include <intravenous/graph/builder/annotations.h>

#include <intravenous/graph/builder/topology.h>
#include <intravenous/graph/builder/virtual_nodes.h>

namespace iv {
void GraphBuilderAnnotations::attach_virtual_node(
    GraphBuilderTopology &topology, GraphBuilderNodeBundles &node_bundles,
    GraphBuilderVirtualNodes &virtual_nodes, size_t node_bundle_handle,
    std::string_view virtual_node_id,
    SourceInfo const *source_info) {
  virtual_nodes.attach_bundle_member(topology, node_bundles, node_bundle_handle,
                                     virtual_node_id, source_info);
}

void GraphBuilderAnnotations::annotate_node_source_info(
    GraphBuilderTopology &topology, GraphBuilderNodeBundles &node_bundles,
    GraphBuilderVirtualNodes &virtual_nodes,
    GraphBuilderIdentity const &identity,
    size_t node_bundle_handle, std::string_view declaration_identity,
    std::string_view file_path, uint32_t begin, uint32_t end) {
  if (declaration_identity.empty()) {
    return;
  }
  if (node_bundle_handle >= node_bundles.size()) {
    details::error("builder " + identity.value +
                   ": cannot record source info for an unknown NodeBundle");
  }
  auto &infos = node_bundles.bundle(node_bundle_handle).source_annotations.infos;
  SourceInfo info{
      .declaration_identity = std::string(declaration_identity),
      .span =
          SourceSpan{
              .file_path = std::string(file_path),
              .begin = begin,
              .end = end,
          },
  };
  if (std::find(infos.begin(), infos.end(), info) == infos.end()) {
    infos.push_back(std::move(info));
  }
  attach_virtual_node(topology, node_bundles, virtual_nodes, node_bundle_handle, declaration_identity,
                      &info);
}
} // namespace iv
