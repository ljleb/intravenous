#pragma once

#include <intravenous/runtime/project_connection_json.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
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
    nlohmann::json state = nlohmann::json::object();

    friend bool operator==(ProjectLaneView const&, ProjectLaneView const&) = default;
};

struct ProjectPresentation {
    std::vector<NodeAddress> lane_order{};
    std::vector<ProjectLaneView> lane_views{};
    nlohmann::json timeline_snap_memory = nlohmann::json::object();
};

inline std::vector<NodeAddress> parse_project_lane_order(
    nlohmann::ordered_json const& order, ProjectConnectionJsonNames names)
{
    if (!order.is_array()) throw std::invalid_argument("lane order must be an array of node addresses");
    std::vector<NodeAddress> result;
    result.reserve(order.size());
    for (auto const& address : order) {
        auto node = parse_node_address_json(address, names);
        if (node.instance_id.empty() || std::ranges::find(result, node) != result.end())
            throw std::invalid_argument("lane order requires unique node addresses");
        result.push_back(std::move(node));
    }
    return result;
}

inline ProjectLaneViewId parse_project_lane_view_id(
    nlohmann::ordered_json const& args, ProjectConnectionJsonNames names)
{
    auto const field = names == ProjectConnectionJsonNames::project_file ? "presentation_id" : "presentationId";
    ProjectLaneViewId id{parse_node_address_json(args.at("address"), names), args.at(field).get<std::string>()};
    if (id.address.instance_id.empty() || id.presentation_id.empty())
        throw std::invalid_argument("lane view requires a node address and presentation ID");
    return id;
}

inline nlohmann::ordered_json project_lane_view_json(
    ProjectLaneView const& view, ProjectConnectionJsonNames names)
{
    auto const field = names == ProjectConnectionJsonNames::project_file ? "presentation_id" : "presentationId";
    return {
        {"address", node_address_json(view.id.address, names)},
        {field, view.id.presentation_id},
        {"state", nlohmann::ordered_json(view.state)},
    };
}

inline nlohmann::json parse_project_presentation_object(nlohmann::ordered_json const& value)
{
    if (!value.is_object()) throw std::invalid_argument("presentation state must be a JSON object");
    return nlohmann::json(value);
}
} // namespace iv
