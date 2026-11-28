#pragma once

#include <intravenous/global_index.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>

namespace iv {

// Positions on one port's native lattice, in forward order from first.
struct SamplePositionRun {
    GlobalIndex first{};
    std::uint64_t count = 0;
    std::uint64_t step = 1;

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return count == 0 || (step != 0
            && count - 1 <= std::numeric_limits<std::uint64_t>::max() / step);
    }

    [[nodiscard]] constexpr GlobalIndex position(std::uint64_t slot) const noexcept
    {
        assert(valid() && slot < count);
        return first.advanced_by(slot * step);
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> slot_at(GlobalIndex value) const noexcept
    {
        assert(valid());
        if (count == 0) return std::nullopt;
        auto const offset = first.forward_distance_to(value);
        auto const slot = offset / step;
        if (slot >= count || offset % step != 0) return std::nullopt;
        return slot;
    }

    constexpr bool operator==(SamplePositionRun const&) const noexcept = default;
};

// The pieces describe a union. Overlapping pieces do not add more samples.
// The owner must keep both sequences alive for the lifetime of the view.
struct SamplePositionsView {
    std::span<SamplePositionRun const> runs{};
    std::span<GlobalIndex const> individual_positions{};

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return individual_positions.empty()
            && std::ranges::all_of(runs, [](auto const& run) { return run.count == 0; });
    }
};

// A circular range can intersect a run in two parts. Visit them in run order
// and retain the original phase, without expanding the selected positions.
template<typename Fn>
constexpr void for_each_sample_position_intersection(
    SamplePositionRun run, GlobalIndexRange range, Fn&& fn)
{
    assert(run.valid());
    if (run.count == 0 || range.empty()) return;

    auto const first = run.first.forward_distance_to(range.begin);
    auto const last = first + (range.extent - 1);
    auto visit = [&](std::uint64_t begin_offset, std::uint64_t end_offset) {
        auto const begin_slot = begin_offset / run.step + (begin_offset % run.step != 0);
        auto const end_slot = std::min(end_offset / run.step, run.count - 1);
        if (end_slot < begin_slot) return;
        fn(SamplePositionRun{
            .first = run.position(begin_slot),
            .count = end_slot - begin_slot + 1,
            .step = run.step,
        });
    };
    if (last < first) {
        visit(0, last);
        visit(first, std::numeric_limits<std::uint64_t>::max());
    } else {
        visit(first, last);
    }
}

// The request was clipped to the span from source.first to its last slot.
// Match its phase against the source without visiting the intervening slots.
[[nodiscard]] inline std::optional<SamplePositionRun> intersect_clipped_sample_run(
    SamplePositionRun source, SamplePositionRun requested)
{
    if (!requested.count || !source.count) return std::nullopt;
    if (requested.count == 1)
        return source.slot_at(requested.first) ? std::optional{requested} : std::nullopt;
    if (source.step == 1) return requested;
    auto const offset = source.first.forward_distance_to(requested.first);
    auto const divisor = std::gcd(source.step, requested.step);
    if (offset % divisor) return std::nullopt;
    auto const period = source.step / divisor;
    std::uint64_t first_slot = 0;
    if (period != 1) {
        // Solve offset + slot * requested.step == 0 modulo source.step.
        // Extended Euclid gives the inverse after dividing by the GCD.
        __int128 coefficient = 0;
        __int128 next_coefficient = 1;
        auto left = period;
        auto right = requested.step / divisor % period;
        while (right) {
            auto const quotient = left / right;
            auto const remainder = left % right;
            left = right;
            right = remainder;
            auto const next = coefficient - static_cast<__int128>(quotient) * next_coefficient;
            coefficient = next_coefficient;
            next_coefficient = next;
        }
        auto const inverse = static_cast<std::uint64_t>(coefficient < 0 ? coefficient + period : coefficient);
        auto const phase = (period - (offset / divisor) % period) % period;
        first_slot = static_cast<std::uint64_t>(static_cast<unsigned __int128>(phase) * inverse % period);
    }
    if (first_slot >= requested.count) return std::nullopt;
    auto const count = 1 + (requested.count - 1 - first_slot) / period;
    return SamplePositionRun{requested.position(first_slot), count, count == 1 ? 1 : requested.step * period};
}

} // namespace iv
