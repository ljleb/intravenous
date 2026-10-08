#include <intravenous/graph_jit/transient_arena_plan.h>

#include <algorithm>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>

namespace iv::graph_jit::detail {
namespace {

bool is_power_of_two(std::size_t value) noexcept
{
    return value != 0 && (value & (value - 1)) == 0;
}

std::expected<std::size_t, std::string> align_up_checked(
    std::size_t value,
    std::size_t alignment)
{
    if (!is_power_of_two(alignment)) {
        return std::unexpected(
            "GraphJit transient arena alignment must be a non-zero power of two");
    }
    if (value > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::unexpected("GraphJit transient arena alignment overflows size_t");
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

std::expected<std::size_t, std::string> end_offset_checked(
    std::size_t offset,
    std::size_t size)
{
    if (size > std::numeric_limits<std::size_t>::max() - offset) {
        return std::unexpected("GraphJit transient arena range overflows size_t");
    }
    return offset + size;
}

struct ActiveRange {
    std::size_t request_index = 0;
    std::size_t offset = 0;
    std::size_t size_bytes = 0;
    std::size_t live_end = 0;
};

} // namespace

std::expected<TransientArenaPlan, std::string> plan_transient_arena(
    std::span<TransientArenaAllocationRequest const> requests)
{
    TransientArenaPlan result;
    result.allocations.resize(requests.size());
    if (requests.empty()) return result;

    std::vector<std::size_t> order(requests.size());
    std::iota(order.begin(), order.end(), std::size_t{0});

    for (std::size_t i = 0; i < requests.size(); ++i) {
        auto const& request = requests[i];
        if (request.size_bytes == 0) {
            return std::unexpected(
                "GraphJit transient arena request must have a non-zero size");
        }
        if (!is_power_of_two(request.alignment)) {
            return std::unexpected(
                "GraphJit transient arena request has an invalid alignment");
        }
        if (request.live_interval.end < request.live_interval.begin) {
            return std::unexpected(
                "GraphJit transient arena request has an inverted live interval");
        }
        if (request.live_interval.crosses_kernel_invocations) {
            return std::unexpected(
                "GraphJit transient arena cannot allocate cross-kernel storage");
        }
    }

    std::stable_sort(
        order.begin(),
        order.end(),
        [&](std::size_t lhs, std::size_t rhs) {
            auto const& a = requests[lhs];
            auto const& b = requests[rhs];
            if (a.live_interval.begin != b.live_interval.begin) {
                return a.live_interval.begin < b.live_interval.begin;
            }
            if (a.size_bytes != b.size_bytes) {
                return a.size_bytes > b.size_bytes;
            }
            if (a.alignment != b.alignment) {
                return a.alignment > b.alignment;
            }
            return lhs < rhs;
        });

    std::vector<ActiveRange> active;
    active.reserve(requests.size());

    for (auto const request_index : order) {
        auto const& request = requests[request_index];

        // Inclusive intervals mean equality still overlaps. Only values ending
        // strictly before this start are dead and cease to occupy arena bytes.
        std::erase_if(active, [&](ActiveRange const& range) {
            return range.live_end < request.live_interval.begin;
        });
        std::sort(active.begin(), active.end(), [](auto const& a, auto const& b) {
            if (a.offset != b.offset) return a.offset < b.offset;
            return a.request_index < b.request_index;
        });

        std::size_t cursor = 0;
        std::optional<std::size_t> chosen;
        for (auto const& occupied : active) {
            auto aligned = align_up_checked(cursor, request.alignment);
            if (!aligned) return std::unexpected(std::move(aligned.error()));
            if (*aligned <= occupied.offset
                && request.size_bytes <= occupied.offset - *aligned) {
                chosen = *aligned;
                break;
            }
            auto occupied_end = end_offset_checked(
                occupied.offset, occupied.size_bytes);
            if (!occupied_end) {
                return std::unexpected(std::move(occupied_end.error()));
            }
            cursor = std::max(cursor, *occupied_end);
        }
        if (!chosen) {
            auto aligned = align_up_checked(cursor, request.alignment);
            if (!aligned) return std::unexpected(std::move(aligned.error()));
            chosen = *aligned;
        }

        auto range_end = end_offset_checked(*chosen, request.size_bytes);
        if (!range_end) return std::unexpected(std::move(range_end.error()));

        result.allocations[request_index] = TransientArenaAllocationPlan{
            .offset = *chosen,
            .size_bytes = request.size_bytes,
            .alignment = request.alignment,
        };
        result.size_bytes = std::max(result.size_bytes, *range_end);
        result.alignment = std::max(result.alignment, request.alignment);
        active.push_back(ActiveRange{
            .request_index = request_index,
            .offset = *chosen,
            .size_bytes = request.size_bytes,
            .live_end = request.live_interval.end,
        });
    }

    return result;
}

} // namespace iv::graph_jit::detail
