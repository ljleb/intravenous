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

[[nodiscard]] std::vector<ProjectNodePathMatch> resolve_project_node_path(
    ConfiguredGraph const& graph,
    NodeBundleHandle subject,
    std::span<ProjectNodePathSelector const> path);

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
