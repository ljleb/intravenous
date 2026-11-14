#pragma once

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/project_connection_types.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iv {
struct ProjectNodePathMatch {
    enum class Kind { bundle, virtual_node } kind = Kind::bundle;
    NodeBundleHandle bundle = 0;
    VirtualNodeHandle virtual_node = 0;
    // Indices into VirtualNodeRecord::node_bundle_handles that belong directly
    // to the selected subject scope. This prevents one scoped lookup from
    // exposing members of the same record that live in a sibling scope.
    std::vector<std::size_t> virtual_member_indices{};

    bool operator==(ProjectNodePathMatch const&) const = default;
};

// The two graph tables a semantic path consults. A live builder session and
// a finished ConfiguredGraph both provide them, so configuration references
// and project connections share one resolver.
struct ProjectNodePathGraph {
    GraphBuilderNodeBundles const& node_bundles;
    GraphBuilderVirtualNodes const& virtual_nodes;
};

[[nodiscard]] std::vector<ProjectNodePathMatch> resolve_project_node_path(
    ProjectNodePathGraph graph,
    NodeBundleHandle subject,
    std::span<ProjectNodePathSelector const> path);

[[nodiscard]] std::vector<ProjectNodePathMatch> resolve_project_node_path(
    ConfiguredGraph const& graph,
    NodeBundleHandle subject,
    std::span<ProjectNodePathSelector const> path);

[[nodiscard]] std::vector<ProjectNodePathMatch> resolve_node_address(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    NodeAddress const& address);

// Expand a resolved semantic match into the concrete NodeBundle handles to
// which one handler delivery would be addressed. Order follows the authored
// virtual-member order and duplicates are removed without reordering.
[[nodiscard]] std::vector<NodeBundleHandle> concrete_node_address_matches(
    ConfiguredGraph const& graph,
    std::span<ProjectNodePathMatch const> matches);

// Resolve the durable project instance ID first, then apply the same path
// operations used by GraphConnections. An unknown instance ID has no matches.
[[nodiscard]] std::vector<ProjectNodePathMatch> resolve_project_node_path(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    std::string_view instance_id,
    std::span<ProjectNodePathSelector const> path);

[[nodiscard]] ProjectNodePathMatch require_single_project_node_path_match(
    std::span<ProjectNodePathMatch const> matches,
    std::string_view description);
} // namespace iv
