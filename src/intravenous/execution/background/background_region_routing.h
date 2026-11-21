#pragma once

#include <intravenous/global_index.h>
#include <intravenous/graph_jit/background_evaluation_plan.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace iv {

struct BackgroundInputRegionSource {
    graph_jit::BackgroundPortIndex output = 0;
    // Replay windows follow the request and have no authored Region index.
    std::optional<std::size_t> region_index{};
    // Sample source channel or event delivery index in the connection.
    std::optional<std::size_t> route_index{};

    bool operator==(BackgroundInputRegionSource const&) const = default;
};

struct BackgroundInputRegion {
    GlobalIndexRange range{};
    graph_jit::BackgroundConnectionIndex connection = 0;
    std::optional<std::size_t> sample_projection{};
    std::vector<BackgroundInputRegionSource> sources{};
    std::size_t logical_index = 0;

    bool operator==(BackgroundInputRegion const&) const = default;
};

template<typename Region>
struct SampleRegionProjectionSource {
    std::span<Region const> regions;
    std::size_t read_latency = 0;
};

// Each intersection retains one source Region index per route. Equal ranges
// from different Regions remain separate, in source order. The visitor can
// stop the traversal before it enumerates the remaining intersections.
template<typename Region, typename Visit, typename RangeOf = std::identity>
bool visit_sample_region_projections(
    std::span<SampleRegionProjectionSource<Region> const> sources,
    Visit&& visit, RangeOf range_of = {},
    std::optional<GlobalIndexRange> window = std::nullopt)
{
    if (sources.empty()) return true;
    std::vector<std::size_t> source_regions(sources.size());
    auto walk = [&](auto&& self, std::size_t group,
                    std::optional<GlobalIndexRange> selected) -> bool {
        if (group == sources.size())
            return visit(*selected, std::span<std::size_t const>{source_regions});
        auto const& source = sources[group];
        for (std::size_t index = 0; index < source.regions.size(); ++index) {
            auto range = std::invoke(range_of, source.regions[index]);
            if (range.empty()) continue;
            range.begin = range.begin.advanced_by(source.read_latency);
            source_regions[group] = index;
            if (!selected) {
                if (!self(self, group + 1, range)) return false;
            } else {
                bool more = true;
                for_each_range_intersection(*selected, range, [&](auto part) {
                    if (more) more = self(self, group + 1, part);
                });
                if (!more) return false;
            }
        }
        return true;
    };
    return walk(walk, 0, window);
}

} // namespace iv
