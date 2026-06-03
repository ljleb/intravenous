#pragma once

#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_refs.h>

#include <cstdint>
#include <string_view>

namespace iv {
    class GraphBuilder;
    class GraphBuilderTopology;
    class GraphBuilderAnnotations {
    public:
        void add_logical_id(GraphBuilderTopology&, size_t node_index, std::string_view logical_node_id);
        void initialize_vacant_input_owner(GraphBuilderTopology&, size_t node_index, std::string_view logical_node_id);
        void transfer_vacant_input_owner(GraphBuilderTopology&, size_t node_index, std::string_view logical_node_id);
        void annotate_node_source_info(
            GraphBuilderTopology&,
            GraphBuilderIdentity const&,
            GraphBuilder const& owner,
            NodeRef const& ref,
            std::string_view declaration_identity,
            std::string_view file_path,
            uint32_t begin,
            uint32_t end
        );
    };
}
