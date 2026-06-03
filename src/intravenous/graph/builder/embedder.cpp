#include <intravenous/graph/builder/embedder.h>

#include <intravenous/graph/compiler.h>
#include <intravenous/graph/builder/connections.h>
#include <intravenous/graph/builder/detach.h>
#include <intravenous/graph/builder/public_ports.h>
#include <intravenous/graph/builder/topology.h>

namespace iv {
size_t GraphBuilderChildEmbedder::embed(
    GraphBuilderTopology& parent_topology,
    GraphBuilderConnections& parent_connections,
    GraphBuilderDetach& parent_detach,
    GraphBuilderPublicPorts const& child_public_ports,
    GraphBuilderTopology const& child_topology,
    GraphBuilderConnections const& child_connections,
    GraphBuilderDetach const& child_detach
)
{
    size_t const child_detach_offset = parent_detach.reserve_child_offset(child_detach);
    size_t const placeholder_node = parent_topology.append_embedded_child(
        child_topology,
        child_public_ports.sample_inputs(),
        child_public_ports.sample_outputs(),
        child_public_ports.event_inputs(),
        child_public_ports.event_outputs(),
        child_detach_offset
    );
    size_t const child_node_offset = placeholder_node + 1;

    parent_connections.import_child(child_connections, child_node_offset);
    parent_detach.import_child(child_detach, child_node_offset, child_detach_offset);
    return placeholder_node;
}
}
