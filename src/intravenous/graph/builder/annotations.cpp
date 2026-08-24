#include <intravenous/graph/builder/annotations.h>

#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/builder/virtual_nodes.hpp>

#include <algorithm>

namespace iv {
void GraphBuilderAnnotations::attach_virtual_node(
    GraphBuilderNodeBundles& bundles, GraphBuilderVirtualNodes& virtual_nodes,
    size_t bundle, std::string_view virtual_node_id,
    SourceInfo const* source_info) {
  virtual_nodes.attach_bundle_member(bundles, bundle, virtual_node_id, source_info);
}

void GraphBuilderAnnotations::annotate_node_source_info(
    GraphBuilderNodeBundles& bundles, GraphBuilderVirtualNodes& virtual_nodes,
    GraphBuilderIdentity const& identity, size_t bundle,
    std::string_view declaration_identity, std::string_view file_path,
    uint32_t begin, uint32_t end) {
  if (declaration_identity.empty()) return;
  if (bundle >= bundles.size())
    details::error("builder " + identity.value +
                   ": cannot record source info for an unknown NodeBundle");
  SourceInfo info{
      .declaration_identity = std::string(declaration_identity),
      .span = {.file_path = std::string(file_path), .begin = begin, .end = end},
  };
  auto& infos = bundles.bundle(bundle).source_annotations().infos;
  if (!std::ranges::contains(infos, info)) infos.push_back(info);
  attach_virtual_node(bundles, virtual_nodes, bundle, declaration_identity, &info);
}
} // namespace iv
