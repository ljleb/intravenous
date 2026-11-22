#pragma once

#include <intravenous/project/persistence/project_connection_json.h>
#include <intravenous/project/project_presentation.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace iv {
inline std::vector<ProjectLaneParent> parse_project_lane_parents(
    nlohmann::ordered_json const& values, ProjectConnectionJsonNames names)
{
    if (!values.is_array()) throw std::invalid_argument("lane parents must be an array");
    std::vector<ProjectLaneParent> parents;
    auto const field = names == ProjectConnectionJsonNames::project_file ? "instance_id" : "instanceId";
    for (auto const& value : values)
        parents.push_back({value.at(field).get<std::string>(), parse_node_address_json(value.at("parent"), names)});
    validate_project_lane_parents(parents);
    return parents;
}

inline nlohmann::ordered_json project_lane_parents_json(
    std::vector<ProjectLaneParent> const& parents, ProjectConnectionJsonNames names)
{
    auto values = nlohmann::ordered_json::array();
    auto const field = names == ProjectConnectionJsonNames::project_file ? "instance_id" : "instanceId";
    for (auto const& placement : parents)
        values.push_back({{field, placement.instance_id}, {"parent", node_address_json(placement.parent, names)}});
    return values;
}

inline std::vector<NodeAddress> parse_project_lane_order(
    nlohmann::ordered_json const& order, ProjectConnectionJsonNames names)
{
    if (!order.is_array()) throw std::invalid_argument("lane order must be an array of node addresses");
    std::vector<NodeAddress> result;
    result.reserve(order.size());
    for (auto const& address : order) {
        auto node = parse_node_address_json(address, names);
        if (std::ranges::find(result, node) != result.end())
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
    if (id.presentation_id.empty())
        throw std::invalid_argument("lane view requires a presentation ID");
    return id;
}

inline nlohmann::ordered_json project_lane_view_json(
    ProjectLaneView const& view, ProjectConnectionJsonNames names)
{
    auto const field = names == ProjectConnectionJsonNames::project_file ? "presentation_id" : "presentationId";
    return {
        {"address", node_address_json(view.id.address, names)},
        {field, view.id.presentation_id},
        {"state", nlohmann::ordered_json::parse(view.state_json)},
    };
}

inline std::string encode_project_presentation_object(nlohmann::ordered_json const& value)
{
    if (!value.is_object()) throw std::invalid_argument("presentation state must be a JSON object");
    return nlohmann::json(value).dump();
}
} // namespace iv
