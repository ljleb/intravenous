#pragma once

#include <intravenous/node/address.h>

#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iv {
struct ProjectLaneViewId {
    NodeAddress address{};
    std::string presentation_id{};

    friend bool operator==(ProjectLaneViewId const&, ProjectLaneViewId const&) = default;
};

struct ProjectLaneView {
    ProjectLaneViewId id{};
    // Validated JSON objects encoded at the persistence and RPC boundaries.
    std::string state_json{"{}"};

    friend bool operator==(ProjectLaneView const&, ProjectLaneView const&) = default;
};

struct ProjectLaneParent {
    std::string instance_id{};
    NodeAddress parent{};
    bool operator==(ProjectLaneParent const&) const = default;
};

inline void validate_project_lane_parents(std::vector<ProjectLaneParent> const& parents)
{
    struct Link { std::string_view parent; bool visited = false; };
    std::unordered_map<std::string_view, Link> links;
    for (auto const& placement : parents) {
        if (placement.instance_id.empty() || placement.parent.instance_id.empty()
            || !links.emplace(placement.instance_id, Link{placement.parent.instance_id}).second)
            throw std::invalid_argument("lane parents require unique project instance IDs");
    }
    for (auto const& placement : parents) {
        auto current = links.find(placement.instance_id);
        while (current != links.end() && !current->second.parent.empty()) {
            auto& link = current->second;
            if (link.visited) throw std::invalid_argument("lane nesting contains a cycle");
            link.visited = true;
            current = links.find(link.parent);
        }
        // The checked path ends at a root or an already checked path.
        current = links.find(placement.instance_id);
        while (current != links.end() && !current->second.parent.empty())
            current = links.find(std::exchange(current->second.parent, std::string_view{}));
    }
}

struct ProjectPresentation {
    std::vector<NodeAddress> lane_order{};
    std::vector<ProjectLaneParent> lane_parents{};
    std::vector<ProjectLaneView> lane_views{};
    std::string timeline_snap_memory_json{"{}"};
};
} // namespace iv
