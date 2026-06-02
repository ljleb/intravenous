#pragma once

#include "../build_types.h"
#include "../node.h"

namespace iv {
    struct GraphBuilderIdentity;
    class GraphBuilderTopology;
    class GraphBuilderConnections;
    class GraphBuilderPublicPorts;
    class GraphBuilderDetach;

    struct GraphBuilderRootNodeBuildResult {
        Graph graph;
        GraphBuildMetadata metadata;
    };

    class GraphBuilderFinalizer {
    public:
        static GraphIntrospectionMetadata build_metadata(
            GraphBuilderIdentity const& identity,
            GraphBuilderTopology const& topology,
            size_t detach_id_offset
        );
        static GraphBuilderRootNodeBuildResult build_root_node(
            GraphBuilderIdentity const& identity,
            GraphBuilderTopology const& topology,
            GraphBuilderConnections const& connections,
            GraphBuilderPublicPorts const& public_ports,
            GraphBuilderDetach const& detach,
            size_t detach_id_offset
        );
    };
}
