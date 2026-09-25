#pragma once

#include <intravenous/channel_ports.h>
#include <intravenous/ports.h>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace iv {
// Structured recursive selectors for persistent whole-project node paths.
// These are semantic project identities; they never store builder-local
// NodeBundle handles.
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
    std::size_t index = 0;
    bool operator==(ProjectTiledChildSelector const&) const = default;
};

// Select one direct nested subgraph scope. `index` is counted after applying
// the optional kind filter and therefore remains distinct from a tiled-child
// index or a port-channel index.
struct ProjectSubgraphSelector {
    std::optional<std::string> kind{};
    std::size_t index = 0;
    bool operator==(ProjectSubgraphSelector const&) const = default;
};

using ProjectNodePathSelector = std::variant<
    ProjectVirtualNodeSelector,
    ProjectVirtualMemberSelector,
    ProjectTiledChildSelector,
    ProjectSubgraphSelector>;

// Omitting name and index means "all ports" of the requested kind/direction.
// Supplying both requires both predicates to match. `channel` is a semantic
// sample-channel selector and is deliberately separate from tiled-child path
// selection.
struct ProjectPortMatcher {
    std::optional<std::string> name{};
    std::optional<std::size_t> index{};
    std::optional<std::size_t> channel{};
    bool operator==(ProjectPortMatcher const&) const = default;
};

struct ProjectNodePortMatcher {
    std::string instance_id{};
    std::vector<ProjectNodePathSelector> path{};
    ProjectPortMatcher port{};
    bool operator==(ProjectNodePortMatcher const&) const = default;
};

struct ProjectSampleConnection {
    std::string connection_id{};
    ChannelTypeId source_type = ChannelTypeId::mono;
    std::vector<ProjectNodePortMatcher> outputs{};
    ChannelTypeId target_type = ChannelTypeId::mono;
    std::vector<ProjectNodePortMatcher> inputs{};
    bool operator==(ProjectSampleConnection const&) const = default;
};

struct ProjectEventConnection {
    std::string connection_id{};
    EventTypeId source_type = EventTypeId::empty;
    std::vector<ProjectNodePortMatcher> outputs{};
    EventTypeId target_type = EventTypeId::empty;
    std::vector<ProjectNodePortMatcher> inputs{};
    bool operator==(ProjectEventConnection const&) const = default;
};

using ProjectConnection = std::variant<ProjectSampleConnection, ProjectEventConnection>;

[[nodiscard]] inline std::string const& project_connection_id(ProjectConnection const& connection)
{
    return std::visit([](auto const& value) -> std::string const& {
        return value.connection_id;
    }, connection);
}
} // namespace iv
