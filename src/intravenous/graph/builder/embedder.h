#pragma once

#include <cstddef>

namespace iv {
    class GraphBuilderTopology;
    class GraphBuilderConnections;
    class GraphBuilderPublicPorts;
    class GraphBuilderDetach;

    class GraphBuilderChildEmbedder {
    public:
        static size_t embed(
            GraphBuilderTopology& parent_topology,
            GraphBuilderConnections& parent_connections,
            GraphBuilderDetach& parent_detach,
            GraphBuilderPublicPorts const& child_public_ports,
            GraphBuilderTopology const& child_topology,
            GraphBuilderConnections const& child_connections,
            GraphBuilderDetach const& child_detach
        );
    };
}
