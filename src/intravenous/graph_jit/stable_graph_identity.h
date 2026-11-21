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

// A persistent identity exists only when the configured concrete node belongs
// to a stable virtual node. Anonymous concrete nodes deliberately receive no
// substitute identity derived from a generation-local bundle handle.
struct StableConcreteNodeId {
    std::string graph{};
    std::string virtual_node{};
    std::size_t direct_member = 0;

    bool operator==(StableConcreteNodeId const&) const = default;
    auto operator<=>(StableConcreteNodeId const&) const = default;
};

// The concrete node's fields of a migration identity string. Each string is
// prefixed with its length, so two different nodes never encode alike.
inline std::string migration_identity_fields(StableConcreteNodeId const& node)
{
    return "graph=" + std::to_string(node.graph.size()) + ':' + node.graph
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
