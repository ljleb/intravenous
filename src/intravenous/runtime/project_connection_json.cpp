#include <intravenous/runtime/project_connection_json.h>

#include <intravenous/graph/names.h>

#include <nlohmann/json.hpp>

#include <limits>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;

struct Fields {
    std::string_view connection_id;
    std::string_view source_type;
    std::string_view target_type;
    std::string_view instance_id;
    std::string_view source_identity;
    std::string_view type_identity;
};

Fields fields(ProjectConnectionJsonNames names)
{
    if (names == ProjectConnectionJsonNames::socket_rpc) {
        return {
            .connection_id = "connectionId",
            .source_type = "sourceType",
            .target_type = "targetType",
            .instance_id = "instanceId",
            .source_identity = "sourceIdentity",
            .type_identity = "typeIdentity",
        };
    }
    return {
        .connection_id = "connection_id",
        .source_type = "source_type",
        .target_type = "target_type",
        .instance_id = "instance_id",
        .source_identity = "source_identity",
        .type_identity = "type_identity",
    };
}

std::string require_string(Json const& json, std::string_view key)
{
    auto const found = json.find(std::string(key));
    if (found == json.end() || !found->is_string()) {
        throw std::runtime_error(
            "project connection JSON is missing string field '" + std::string(key) + "'");
    }
    return found->get<std::string>();
}

std::optional<std::string> optional_string(Json const& json, std::string_view key)
{
    auto const found = json.find(std::string(key));
    if (found == json.end() || found->is_null()) return std::nullopt;
    if (!found->is_string()) {
        throw std::runtime_error(
            "project connection JSON field '" + std::string(key)
            + "' must be a string or null");
    }
    return found->get<std::string>();
}

std::optional<std::size_t> optional_index(Json const& json, std::string_view key)
{
    auto const found = json.find(std::string(key));
    if (found == json.end() || found->is_null()) return std::nullopt;
    std::uint64_t value = 0;
    if (found->is_number_unsigned()) {
        value = found->get<std::uint64_t>();
    } else if (found->is_number_integer()) {
        auto const signed_value = found->get<std::int64_t>();
        if (signed_value < 0) {
            throw std::runtime_error(
                "project connection JSON field '" + std::string(key)
                + "' must be an unsigned integer or null");
        }
        value = static_cast<std::uint64_t>(signed_value);
    } else {
        throw std::runtime_error(
            "project connection JSON field '" + std::string(key)
            + "' must be an unsigned integer or null");
    }
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(
            "project connection JSON field '" + std::string(key) + "' is too large");
    }
    return static_cast<std::size_t>(value);
}

ChannelTypeId parse_channel_type(std::string_view name)
{
    for (auto const& info : channel_type_registry) {
        if (info.wire_name == name) return info.id;
    }
    throw std::runtime_error("unknown project connection channel type: " + std::string(name));
}

EventTypeId parse_event_type(std::string_view name)
{
    for (auto const type : {
             EventTypeId::empty,
             EventTypeId::trigger,
             EventTypeId::boundary,
             EventTypeId::midi,
             EventTypeId::sample,
         }) {
        if (details::event_type_name(type) == name) return type;
    }
    throw std::runtime_error("unknown project connection event type: " + std::string(name));
}

Json selector_json(ProjectNodePathSelector const& selector, Fields const& names)
{
    return std::visit([&](auto const& value) -> Json {
        using Selector = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<Selector, ProjectVirtualNodeSelector>) {
            Json result{
                {"kind", "virtualNode"},
                {std::string(names.source_identity), value.source_identity},
            };
            if (value.type_identity) {
                result[std::string(names.type_identity)] = *value.type_identity;
            }
            return result;
        } else if constexpr (std::same_as<Selector, ProjectVirtualMemberSelector>) {
            return Json{{"kind", "virtualMember"}, {"index", value.index}};
        } else {
            return Json{{"kind", "tiledChild"}, {"name", value.name}};
        }
    }, selector);
}

ProjectNodePathSelector parse_selector(Json const& json, Fields const& names)
{
    if (!json.is_object()) {
        throw std::runtime_error("project connection path selector must be an object");
    }
    auto const kind = require_string(json, "kind");
    if (kind == "virtualNode") {
        return ProjectVirtualNodeSelector{
            .source_identity = require_string(json, names.source_identity),
            .type_identity = optional_string(json, names.type_identity),
        };
    }
    if (kind == "virtualMember") {
        auto const index = optional_index(json, "index");
        if (!index) {
            throw std::runtime_error(
                "project virtual-member selector requires unsigned field 'index'");
        }
        return ProjectVirtualMemberSelector{.index = *index};
    }
    if (kind == "tiledChild") {
        return ProjectTiledChildSelector{.name = require_string(json, "name")};
    }
    throw std::runtime_error("unknown project connection path selector kind: " + kind);
}

Json matcher_json(ProjectNodePortMatcher const& matcher, Fields const& names)
{
    Json path = Json::array();
    for (auto const& selector : matcher.path) {
        path.push_back(selector_json(selector, names));
    }
    Json port = Json::object();
    if (matcher.port.name) port["name"] = *matcher.port.name;
    if (matcher.port.index) port["index"] = *matcher.port.index;
    if (matcher.port.channel) port["channel"] = *matcher.port.channel;
    return Json{
        {std::string(names.instance_id), matcher.instance_id},
        {"path", std::move(path)},
        {"port", std::move(port)},
    };
}

ProjectNodePortMatcher parse_matcher(Json const& json, Fields const& names)
{
    if (!json.is_object()) {
        throw std::runtime_error("project connection matcher must be an object");
    }
    auto const path = json.find("path");
    auto const port = json.find("port");
    if (path == json.end() || !path->is_array()) {
        throw std::runtime_error("project connection matcher requires array field 'path'");
    }
    if (port == json.end() || !port->is_object()) {
        throw std::runtime_error("project connection matcher requires object field 'port'");
    }
    ProjectNodePortMatcher result{
        .instance_id = require_string(json, names.instance_id),
        .port = {
            .name = optional_string(*port, "name"),
            .index = optional_index(*port, "index"),
            .channel = optional_index(*port, "channel"),
        },
    };
    result.path.reserve(path->size());
    for (auto const& selector : *path) {
        result.path.push_back(parse_selector(selector, names));
    }
    return result;
}

Json matchers_json(
    std::vector<ProjectNodePortMatcher> const& matchers,
    Fields const& names)
{
    Json result = Json::array();
    for (auto const& matcher : matchers) result.push_back(matcher_json(matcher, names));
    return result;
}

std::vector<ProjectNodePortMatcher> parse_matchers(
    Json const& json,
    std::string_view field,
    Fields const& names)
{
    auto const found = json.find(std::string(field));
    if (found == json.end() || !found->is_array()) {
        throw std::runtime_error(
            "project connection JSON requires array field '" + std::string(field) + "'");
    }
    std::vector<ProjectNodePortMatcher> result;
    result.reserve(found->size());
    for (auto const& matcher : *found) result.push_back(parse_matcher(matcher, names));
    return result;
}
} // namespace

nlohmann::ordered_json project_connection_json(
    ProjectConnection const& connection,
    ProjectConnectionJsonNames name_style)
{
    auto const names = fields(name_style);
    return std::visit([&](auto const& value) -> Json {
        using Connection = std::remove_cvref_t<decltype(value)>;
        Json result{
            {"kind", std::same_as<Connection, ProjectSampleConnection> ? "sample" : "event"},
            {std::string(names.connection_id), value.connection_id},
            {"outputs", matchers_json(value.outputs, names)},
            {"inputs", matchers_json(value.inputs, names)},
        };
        if constexpr (std::same_as<Connection, ProjectSampleConnection>) {
            result[std::string(names.source_type)] = std::string(
                channel_type_info(value.source_type).wire_name);
            result[std::string(names.target_type)] = std::string(
                channel_type_info(value.target_type).wire_name);
        } else {
            result[std::string(names.source_type)] =
                details::event_type_name(value.source_type);
            result[std::string(names.target_type)] =
                details::event_type_name(value.target_type);
        }
        return result;
    }, connection);
}

ProjectConnection parse_project_connection_json(
    nlohmann::ordered_json const& json,
    ProjectConnectionJsonNames name_style)
{
    if (!json.is_object()) {
        throw std::runtime_error("project connection must be a JSON object");
    }
    auto const names = fields(name_style);
    auto const kind = require_string(json, "kind");
    auto const connection_id = require_string(json, names.connection_id);
    auto outputs = parse_matchers(json, "outputs", names);
    auto inputs = parse_matchers(json, "inputs", names);
    if (kind == "sample") {
        return ProjectSampleConnection{
            .connection_id = connection_id,
            .source_type = parse_channel_type(require_string(json, names.source_type)),
            .outputs = std::move(outputs),
            .target_type = parse_channel_type(require_string(json, names.target_type)),
            .inputs = std::move(inputs),
        };
    }
    if (kind == "event") {
        return ProjectEventConnection{
            .connection_id = connection_id,
            .source_type = parse_event_type(require_string(json, names.source_type)),
            .outputs = std::move(outputs),
            .target_type = parse_event_type(require_string(json, names.target_type)),
            .inputs = std::move(inputs),
        };
    }
    throw std::runtime_error("unknown project connection kind: " + kind);
}

nlohmann::ordered_json node_address_json(
    NodeAddress const& address,
    ProjectConnectionJsonNames name_style)
{
    auto const names = fields(name_style);
    Json path = Json::array();
    for (auto const& selector : address.path) {
        path.push_back(selector_json(selector, names));
    }
    return Json{
        {std::string(names.instance_id), address.instance_id},
        {"path", std::move(path)},
    };
}

NodeAddress parse_node_address_json(
    nlohmann::ordered_json const& json,
    ProjectConnectionJsonNames name_style)
{
    if (!json.is_object()) {
        throw std::runtime_error("node address must be a JSON object");
    }
    auto const names = fields(name_style);
    auto const path = json.find("path");
    if (path == json.end() || !path->is_array()) {
        throw std::runtime_error("node address requires array field 'path'");
    }
    NodeAddress result{.instance_id = require_string(json, names.instance_id)};
    if (result.instance_id.empty()) {
        throw std::runtime_error("node address instance id must not be empty");
    }
    result.path.reserve(path->size());
    for (auto const& selector : *path) {
        result.path.push_back(parse_selector(selector, names));
    }
    return result;
}
} // namespace iv
