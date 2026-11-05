#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace iv {
// Stable semantic path components. They identify authored nodes and never
// contain a GraphBuilder-local NodeBundleHandle or a compiled node index.
struct ProjectVirtualNodeSelector {
    std::string source_identity{};
    std::optional<std::string> type_identity{};
    bool operator==(ProjectVirtualNodeSelector const&) const = default;
};

struct ProjectVirtualMemberSelector {
    std::size_t index = 0;
    bool operator==(ProjectVirtualMemberSelector const&) const = default;
};

struct ProjectTiledChildSelector {
    std::string name{};
    bool operator==(ProjectTiledChildSelector const&) const = default;
};

using ProjectNodePathSelector = std::variant<
    ProjectVirtualNodeSelector,
    ProjectVirtualMemberSelector,
    ProjectTiledChildSelector>;

// Serializable semantic identity rooted at one durable project instance.
// A path can intentionally select a set. Runtime dispatch expands that set
// against one immutable ProjectGraphGeneration before invoking handlers.
struct NodeAddress {
    std::string instance_id{};
    std::vector<ProjectNodePathSelector> path{};

    bool operator==(NodeAddress const&) const = default;
};
} // namespace iv
