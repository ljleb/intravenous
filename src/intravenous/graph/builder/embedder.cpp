#include <intravenous/graph/builder/embedder.h>
#include <intravenous/graph/builder/connections.h>
#include <intravenous/graph/builder/detach.h>
#include <intravenous/graph/builder/public_ports.h>
#include <intravenous/graph/builder/node_bundles.h>
#include <intravenous/graph/builder/virtual_nodes.h>

namespace iv {
size_t GraphBuilderChildEmbedder::embed(
    GraphBuilderNodeBundles& parent_bundles,
    GraphBuilderConnections& parent_connections,
    GraphBuilderDetach& parent_detach,
    GraphBuilderVirtualNodes& parent_virtual_nodes,
    GraphBuilderPublicPorts const&,
    GraphBuilderNodeBundles const& child_bundles,
    GraphBuilderConnections const& child_connections,
    GraphBuilderDetach const& child_detach,
    GraphBuilderVirtualNodes const& child_virtual_nodes) {
  auto const detach_offset = parent_detach.reserve_child_offset(child_detach);
  auto const bundle_offset = parent_bundles.import_child(child_bundles, detach_offset);
  parent_connections.import_child(child_connections, bundle_offset);
  parent_detach.import_child(child_detach, bundle_offset, detach_offset);
  parent_virtual_nodes.import_child(parent_bundles, child_virtual_nodes, bundle_offset);
  return bundle_offset;
}
} // namespace iv
