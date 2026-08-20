#include <intravenous/graph/builder/embedder.h>

#include <intravenous/graph/builder/connections.h>
#include <intravenous/graph/builder/detach.h>
#include <intravenous/graph/builder/public_ports.h>
#include <intravenous/graph/builder/topology.h>
#include <intravenous/graph/builder/node_bundles.h>
#include <intravenous/graph/builder/virtual_nodes.h>
#include <intravenous/graph/compiler.h>

namespace iv {
size_t GraphBuilderChildEmbedder::embed(
    GraphBuilderTopology &parent_topology,
    GraphBuilderNodeBundles &parent_node_bundles,
    GraphBuilderConnections &parent_connections,
    GraphBuilderDetach &parent_detach,
    GraphBuilderVirtualNodes &parent_virtual_nodes,
    GraphBuilderPublicPorts const &child_public_ports,
    GraphBuilderTopology const &child_topology,
    GraphBuilderNodeBundles const &child_node_bundles,
    GraphBuilderConnections const &child_connections,
    GraphBuilderDetach const &child_detach,
    GraphBuilderVirtualNodes const &child_virtual_nodes) {
  size_t const child_detach_offset =
      parent_detach.reserve_child_offset(child_detach);
  size_t const subgraph_node = parent_topology.append_embedded_child(
      child_topology, child_public_ports.sample_inputs(child_node_bundles),
      child_public_ports.sample_outputs(child_node_bundles),
      child_public_ports.event_inputs(child_node_bundles),
      child_public_ports.event_outputs(child_node_bundles), child_detach_offset);
  size_t const child_node_offset = subgraph_node + 1;
  size_t const child_node_bundle_offset = parent_node_bundles.size();

  parent_node_bundles.import_child(
      child_node_bundles, child_node_offset, child_detach_offset);
  parent_connections.import_child(
      child_connections, child_node_offset, child_node_bundle_offset);
  parent_detach.import_child(
      child_detach, child_node_bundle_offset, child_detach_offset);
  parent_virtual_nodes.import_child(parent_topology, parent_node_bundles,
                                    child_virtual_nodes, child_node_offset,
                                    child_node_bundle_offset);
  return subgraph_node;
}
} // namespace iv
