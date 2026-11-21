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

// The process-local identity of one original configured snapshot. Generated
// execution bundles are outside this inventory. A zero identity is unscoped.
struct ConfiguredBundleInventory {
    std::uint64_t snapshot = 0;
    std::size_t size = 0;
    bool operator==(ConfiguredBundleInventory const&) const = default;
};

// An authored identity names a project leaf instance or a virtual node and
// direct member. Anonymous concrete nodes have a bundle coordinate only within
// one configured snapshot.
struct StableConcreteNodeId {
    std::string graph{};
    std::string virtual_node{};
    std::size_t direct_member = 0;
    std::string project_instance{};
    std::string project_leaf_type{};
    // A configured bundle coordinate cannot pair across snapshots.
    // Project addresses and package metadata never use this field.
    std::uint64_t configuration_snapshot = 0;

    bool operator==(StableConcreteNodeId const&) const = default;
    auto operator<=>(StableConcreteNodeId const&) const = default;
};

// The concrete node's fields of a migration identity string. Each string is
// prefixed with its length, so two different nodes never encode alike.
inline std::string migration_identity_fields(StableConcreteNodeId const& node)
{
    auto graph = "graph=" + std::to_string(node.graph.size()) + ':' + node.graph;
    if (node.configuration_snapshot)
        return graph + ":snapshot=" + std::to_string(node.configuration_snapshot)
            + ":bundle=" + std::to_string(node.direct_member);
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
