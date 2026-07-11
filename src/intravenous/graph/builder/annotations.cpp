#include <intravenous/graph/builder/annotations.h>

#include <intravenous/graph/builder/topology.h>

namespace iv {
void GraphBuilderAnnotations::add_logical_id(
    GraphBuilderTopology& topology,
    size_t node_index,
    std::string_view logical_node_id
)
{
    if (logical_node_id.empty()) {
        return;
    }
    auto& logical_node_ids = topology.node(node_index).logical_binding.ids;
    if (!std::ranges::contains(logical_node_ids, std::string(logical_node_id))) {
        logical_node_ids.push_back(std::string(logical_node_id));
    }
}

void GraphBuilderAnnotations::initialize_vacant_input_owner(
    GraphBuilderTopology& topology,
    size_t node_index,
    std::string_view logical_node_id
)
{
    if (logical_node_id.empty()) {
        return;
    }
    auto& owner = topology.node(node_index).vacant_input_ownership.logical_node_id;
    if (owner.empty()) {
        owner = std::string(logical_node_id);
    }
}

void GraphBuilderAnnotations::transfer_vacant_input_owner(
    GraphBuilderTopology& topology,
    size_t node_index,
    std::string_view logical_node_id
)
{
    if (logical_node_id.empty()) {
        return;
    }
    topology.node(node_index).vacant_input_ownership.logical_node_id = std::string(logical_node_id);
}

void GraphBuilderAnnotations::annotate_node_source_info(
    GraphBuilderTopology& topology,
    GraphBuilderIdentity const& identity,
    GraphBuilder const& owner,
    NodeRef const& ref,
    std::string_view declaration_identity,
    std::string_view file_path,
    uint32_t begin,
    uint32_t end
)
{
    if (!ref._graph_builder || declaration_identity.empty()) {
        return;
    }
    if (ref._graph_builder != &owner) {
        details::error(
            "builder " + identity.value + ": cannot record source info for node at address " +
            identity.child_id(ref._index) +
            " because it belongs to another builder"
        );
    }
    auto& infos = topology.node(ref._index).source_annotations.infos;
    SourceInfo info {
        .declaration_identity = std::string(declaration_identity),
        .span = SourceSpan {
            .file_path = std::string(file_path),
            .begin = begin,
            .end = end,
        },
    };
    if (std::find(infos.begin(), infos.end(), info) == infos.end()) {
        infos.push_back(std::move(info));
    }
    add_logical_id(topology, ref._index, declaration_identity);
    initialize_vacant_input_owner(topology, ref._index, declaration_identity);
}
}
