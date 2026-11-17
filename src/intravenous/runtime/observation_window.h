#pragma once

#include <intravenous/runtime/observation_types.h>
#include <intravenous/runtime/background_storage_realization.h>

#include <algorithm>

namespace iv {
// Values are selected deterministically in the requested native output lattice.
// Gaps and overlapping logical Regions remain separate; no fan-in is invented.
inline std::vector<GlobalIndex> observation_window_positions(GlobalIndexRange window, std::size_t count)
{
    std::vector<GlobalIndex> positions;
    if (window.empty() || !count) return positions;
    count = static_cast<std::size_t>(std::min<std::uint64_t>(count, window.extent));
    positions.reserve(count);
    auto const intervals = std::max<std::size_t>(1, count - 1);
    auto const whole = (window.extent - 1) / intervals;
    auto const remainder = (window.extent - 1) % intervals;
    for (std::size_t i = 0; i < count; ++i)
        positions.push_back(window.begin.advanced_by(whole * i + remainder * i / intervals));
    return positions;
}

inline void project_observation_window(ObservationWindowQuery& query,
    GlobalIndexRange window, std::span<GlobalIndex const> positions,
    std::span<GlobalIndexRange const> logical_regions, BackgroundSampleReadView const* view,
    bool positional_recording = false)
{
    auto& result = query.result;
    std::size_t value_count = 0, next_storage_region = 0;
    for (auto const logical : logical_regions) {
        if (query.canceled.load(std::memory_order_relaxed)) return;
        auto clipped = IndexCoverage{logical} & IndexCoverage{window};
        if (clipped.empty()) continue;
        ContinuousRegionStorageView const* storage_region = nullptr;
        if (view) {
            for (auto i = next_storage_region; i < view->regions.size(); ++i) {
                if (view->regions[i].range == logical) {
                    storage_region = &view->regions[i];
                    next_storage_region = i + 1;
                    break;
                }
            }
        }
        for (auto const range : clipped) {
            if (result.regions.size() == maximum_observation_window_values) {
                result.truncated = true;
                return;
            }
            auto& region = result.regions.emplace_back();
            region.range = range;
            for (auto const position : positions) {
                if (!range.contains(position)) continue;
                if (value_count++ == maximum_observation_window_values) {
                    result.truncated = true;
                    return;
                }
                ObservationCaptureState captured;
                auto const* selected_region = storage_region;
                if (positional_recording && view) {
                    auto found = std::ranges::find_if(view->regions,
                        [&](auto const& region) { return region.range.contains(position); });
                    if (found != view->regions.end()) selected_region = &*found;
                }
                if (selected_region && view->has_channel(query.channel)) {
                    if (auto const* value = selected_region->value_storage(position, query.channel))
                        query.operation->capture_point(captured, value);
                }
                region.values.emplace_back(position, captured);
            }
        }
    }
}
} // namespace iv
