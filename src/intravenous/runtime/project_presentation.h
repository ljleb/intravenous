#pragma once

#include <intravenous/node/address.h>

#include <string>
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

struct ProjectPresentation {
    std::vector<NodeAddress> lane_order{};
    std::vector<ProjectLaneView> lane_views{};
    std::string timeline_snap_memory_json{"{}"};
};
} // namespace iv
