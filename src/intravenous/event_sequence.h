#pragma once

#include <intravenous/ports.h>

#include <algorithm>
#include <optional>
#include <iterator>
#include <span>

namespace iv {
struct EventNeighbors {
    // Equal-position events keep their semantic order: the last event at the
    // requested position is the held value, and after is strictly later.
    std::optional<TimedEvent> at_or_before{};
    std::optional<TimedEvent> after{};
};

// The sequence is sorted relative to origin. This ordering crosses either
// the signed ordering seam or the unsigned wrap without changing the search.
[[nodiscard]] inline EventNeighbors sorted_event_neighbors(
    std::span<TimedEvent const> events, GlobalIndex origin, GlobalIndex position)
{
    auto const offset = origin.forward_distance_to(position);
    auto const next = std::ranges::upper_bound(events, offset, {},
        [origin](TimedEvent const& event) { return origin.forward_distance_to(event.time); });
    EventNeighbors result;
    if (next != events.begin()) result.at_or_before = *std::prev(next);
    if (next != events.end()) result.after = *next;
    return result;
}

[[nodiscard]] inline std::span<TimedEvent const> sorted_event_range(
    std::span<TimedEvent const> events, GlobalIndexRange region, GlobalIndexRange requested)
{
    IV_ASSERT(region.contains(requested), "event search lies outside its logical Region");
    if (requested.empty()) return {};
    auto const begin = region.begin.forward_distance_to(requested.begin);
    auto const project = [origin = region.begin](TimedEvent const& event) {
        return origin.forward_distance_to(event.time);
    };
    auto const first = std::ranges::lower_bound(events, begin, {}, project);
    auto const last = std::ranges::lower_bound(events, begin + requested.extent, {}, project);
    return {first, last};
}
} // namespace iv
