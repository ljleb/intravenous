#pragma once

#include <intravenous/graph/builder/constexpr_hash.hpp>
#include <intravenous/graph/types.h>

namespace iv {

struct TopologyPortIdHash {
  constexpr size_t operator()(TopologyPortId const& port) const {
    return details::constexpr_hash_combine(port.node, port.port);
  }
};

namespace details {

struct ConcretePortIdHash {
    constexpr size_t operator()(ConcretePortId const& value) const
    {
        return constexpr_hash_combine(value.node, value.port);
    }
};

struct GraphEdgeHash {
    constexpr size_t operator()(GraphEdge const& edge) const
    {
        return constexpr_hash_combine(
            constexpr_hash_combine(edge.source.node, edge.source.port),
            constexpr_hash_combine(edge.target.node, edge.target.port));
    }
};

struct GraphEventEdgeHash {
    constexpr size_t operator()(GraphEventEdge const& edge) const
    {
        return constexpr_hash_combine(
            constexpr_hash_combine(edge.source.node, edge.source.port),
            constexpr_hash_combine(edge.target.node, edge.target.port));
    }
};

} // namespace details
} // namespace iv
