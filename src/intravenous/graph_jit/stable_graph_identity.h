#pragma once

#include <intravenous/ports.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>

namespace iv::graph_jit {

enum class PortDirection : std::uint8_t {
    input,
    output,
};

// A concrete node is identified by its project leaf instance or by its stable
// virtual node and direct member. Anonymous concrete nodes have neither.
struct StableConcreteNodeId {
    std::string graph{};
    std::string virtual_node{};
    std::size_t direct_member = 0;
    std::string project_instance{};
    std::string project_leaf_type{};

    bool operator==(StableConcreteNodeId const&) const = default;
    auto operator<=>(StableConcreteNodeId const&) const = default;
};

// The concrete node's fields of a migration identity string. Each string is
// prefixed with its length, so two different nodes never encode alike.
inline std::string migration_identity_fields(StableConcreteNodeId const& node)
{
    auto graph = "graph=" + std::to_string(node.graph.size()) + ':' + node.graph;
    if (!node.project_instance.empty()) {
        return graph + ":instance=" + std::to_string(node.project_instance.size()) + ':'
            + node.project_instance + ":type=" + std::to_string(node.project_leaf_type.size())
            + ':' + node.project_leaf_type;
    }
    return graph
        + ":node=" + std::to_string(node.virtual_node.size()) + ':'
        + node.virtual_node
        + ":member=" + std::to_string(node.direct_member);
}

struct StableOutputPortId {
    StableConcreteNodeId node{};
    PortKind kind = PortKind::sample;
    std::string port_name{};
    std::size_t port_index = 0;

    bool operator==(StableOutputPortId const&) const = default;
    auto operator<=>(StableOutputPortId const&) const = default;
};

struct StableInputPortId {
    StableConcreteNodeId node{};
    PortKind kind = PortKind::sample;
    std::string port_name{};
    std::size_t port_index = 0;

    bool operator==(StableInputPortId const&) const = default;
    auto operator<=>(StableInputPortId const&) const = default;
};

} // namespace iv::graph_jit
