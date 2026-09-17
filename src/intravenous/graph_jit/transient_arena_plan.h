#pragma once

#include <intravenous/graph_jit/connection_plan.h>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace iv::graph_jit::detail {

struct TransientArenaAllocationRequest {
    std::size_t size_bytes = 0;
    std::size_t alignment = 1;
    ConnectionLiveIntervalPlan live_interval{};
};

struct TransientArenaAllocationPlan {
    std::size_t offset = 0;
    std::size_t size_bytes = 0;
    std::size_t alignment = 1;

    bool operator==(TransientArenaAllocationPlan const&) const = default;
};

struct TransientArenaPlan {
    // Indexed exactly like the request span supplied to plan_transient_arena().
    std::vector<TransientArenaAllocationPlan> allocations{};
    std::size_t size_bytes = 0;
    std::size_t alignment = 1;

    bool operator==(TransientArenaPlan const&) const = default;
};

// Deterministic compile-time byte-range packing for transient values. Inclusive
// live intervals overlap at a shared schedule position, so a range is reusable
// only when old.end < next.begin. There is no runtime allocator or metadata.
//
// Requests are considered in lifetime-start order. Equal-start requests are
// placed largest/alignment-first to reduce fragmentation. For each request the
// allocator scans only currently-live ranges and selects the lowest aligned gap;
// expired ranges therefore disappear from occupancy and their partial/adjacent
// holes are automatically coalesced.
std::expected<TransientArenaPlan, std::string> plan_transient_arena(
    std::span<TransientArenaAllocationRequest const> requests);

} // namespace iv::graph_jit::detail
