#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace iv::graph_jit {
struct NodeImplementation;
}

namespace iv::graph_jit::detail {

struct PrimitiveBundle {
    std::size_t node_bundle = 0;
    std::size_t node_size = 0;
    std::size_t node_alignment = 1;
    std::size_t maximum_block_size = 0;
    bool block_skippable = false;
    std::optional<std::size_t> ttl_samples{};
};

struct PrimitiveAnalysis {
    PrimitiveBundle bundle{};
    NodeImplementation const* implementation = nullptr;
};

struct GraphAnalysis {
    bool empty = false;
    // Stable primitive inventory consumed by all later phases. For the current
    // disconnected zero-port slice this is configured-bundle order; later
    // schedule/SCC planning may choose a different execution order.
    std::vector<PrimitiveAnalysis> primitives{};
};

} // namespace iv::graph_jit::detail
