#pragma once

#include <intravenous/runtime/background_coverage_propagation.h>
#include <intravenous/runtime/background_region_routing.h>
#include <intravenous/runtime/observation_types.h>
#include <intravenous/runtime/background_storage_realization.h>

#include <algorithm>
#include <cstring>

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

struct CachedObservationValues {
    std::span<GlobalIndex const> positions;
    std::span<ObservationWindowRegion const> regions;

    [[nodiscard]] bool contains(GlobalIndex position) const
    {
        if (!std::ranges::contains(positions, position)) return false;
        // Empty coverage is also an exact answer. Missing values in a
        // covering Region, however, do not prove a sample value.
        for (auto const& region : regions) {
            if (!region.range.contains(position)) continue;
            auto const sample = std::ranges::find(region.values, position,
                &std::pair<GlobalIndex, ObservationCaptureState>::first);
            if (sample == region.values.end() || !sample->second.captured) return false;
        }
        return true;
    }

    [[nodiscard]] ObservationCaptureState const* value(
        std::span<std::size_t const> key, GlobalIndex position) const
    {
        for (auto const& region : regions) {
            if (!region.range.contains(position) || !std::ranges::equal(key, region.region_key)) continue;
            auto const found = std::ranges::find(region.values, position,
                &std::pair<GlobalIndex, ObservationCaptureState>::first);
            if (found != region.values.end() && found->second.captured) return &found->second;
        }
        return nullptr;
    }
};

struct ObservationWindowReuse {
    std::vector<CachedObservationValues> windows;

    [[nodiscard]] bool contains(GlobalIndex position) const
    {
        return std::ranges::any_of(windows, [&](auto const& window) { return window.contains(position); });
    }

    [[nodiscard]] ObservationCaptureState const* value(
        std::span<std::size_t const> key, GlobalIndex position) const
    {
        for (auto const& window : windows)
            if (auto const* sample = window.value(key, position)) return sample;
        return nullptr;
    }
};

inline void project_observation_window(ObservationWindowQuery& query,
    GlobalIndexRange window, std::span<GlobalIndex const> positions,
    std::span<GlobalIndexRange const> logical_regions, BackgroundSampleReadView const* view,
    bool positional_recording = false,
    std::optional<std::span<std::size_t const>> prepared_regions = std::nullopt,
    ObservationWindowReuse const* reuse = nullptr)
{
    auto& result = query.result;
    std::size_t value_count = 0, next_storage_region = 0;
    for (std::size_t index = 0; index < logical_regions.size(); ++index) {
        auto const logical = logical_regions[index];
        if (query.canceled.load(std::memory_order_relaxed)) return;
        auto clipped = IndexCoverage{logical} & IndexCoverage{window};
        if (clipped.empty()) continue;
        ContinuousRegionStorageView const* storage_region = nullptr;
        if (view && prepared_regions) {
            auto const prepared = std::ranges::lower_bound(*prepared_regions, index);
            if (prepared != prepared_regions->end() && *prepared == index) {
                auto const local = static_cast<std::size_t>(prepared - prepared_regions->begin());
                if (local < view->regions.size() && view->regions[local].range == logical)
                    storage_region = &view->regions[local];
            }
        } else if (view) {
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
            region.region_key = {index};
            for (auto const position : positions) {
                if (!range.contains(position)) continue;
                if (value_count++ == maximum_observation_window_values) {
                    result.truncated = true;
                    return;
                }
                ObservationCaptureState captured;
                if (auto const* cached = reuse ? reuse->value(region.region_key, position) : nullptr) {
                    region.values.emplace_back(position, *cached);
                    continue;
                }
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

inline std::optional<graph_jit::PortStorageIndex> observation_output_storage(
    graph_jit::BackgroundEvaluationPlan const& plan, BackgroundStorageRealization const& storage,
    graph_jit::BackgroundPortIndex port, std::size_t channel)
{
    for (graph_jit::PortStorageIndex i = 0; i < plan.storage.ports.size(); ++i) {
        auto const& slot = plan.storage.ports[i];
        if (slot.output_port != port && slot.source_port != plan.ports[port].configured_port) continue;
        if (slot.storage != graph_jit::PortStorageKind::background
            && slot.storage != graph_jit::PortStorageKind::persisted_data) continue;
        auto const* view = storage.sample_read(i);
        if (view && view->has_channel(channel)) return i;
    }
    return std::nullopt;
}

inline void project_public_coverage_input(ObservationWindowQuery& query,
    graph_jit::PublicCoverageInputView const& input, GlobalIndexRange window,
    std::span<GlobalIndex const> positions, graph_jit::BackgroundEvaluationPlan const& plan,
    PreparedCoveragePropagation const& prepared, BackgroundStorageRealization const& storage,
    ObservationWindowReuse const* reuse = nullptr)
{
    std::size_t value_count = 0;
    for (std::size_t projection_index = 0; projection_index < input.projections.size(); ++projection_index) {
        auto const& projection = input.projections[projection_index];
        struct RegionSource {
            graph_jit::BackgroundPortIndex output;
            std::size_t latency;
            std::vector<GlobalIndexRange> ranges;
        };
        std::vector<RegionSource> region_sources;
        std::vector<std::size_t> source_groups;
        std::vector<std::vector<ContinuousRegionStorageView const*>> source_views;
        for (auto const& term : projection.sources) {
            auto const slot = observation_output_storage(plan, storage, term.output, term.channel);
            auto const* data = slot ? storage.sample_read(*slot) : nullptr;
            auto const published = prepared.output_regions(term.output);
            auto const& port = plan.ports[term.output];
            auto const positional = published.empty() || port.replayed_tick_output || port.persisted_tick_output;
            auto group = std::ranges::find_if(region_sources, [&](auto const& candidate) {
                return candidate.output == term.output && candidate.latency == term.read_latency;
            });
            if (group == region_sources.end()) {
                auto& next = region_sources.emplace_back(term.output, term.read_latency, std::vector<GlobalIndexRange>{});
                if (data && positional) {
                    for (auto const& region : data->regions) next.ranges.push_back(region.range);
                } else {
                    next.ranges.assign(published.begin(), published.end());
                }
                group = std::prev(region_sources.end());
            }
            source_groups.push_back(group - region_sources.begin());
            auto& views = source_views.emplace_back();
            views.reserve(group->ranges.size());
            for (std::size_t index = 0; index < group->ranges.size(); ++index) {
                auto const* found = static_cast<ContinuousRegionStorageView const*>(nullptr);
                if (data) {
                    auto const local = positional ? std::optional{index}
                        : storage.prepared_region_index(*slot, index);
                    if (local && *local < data->regions.size()
                        && data->regions[*local].range == group->ranges[index])
                        found = &data->regions[*local];
                }
                views.push_back(found);
            }
        }
        auto const& channels = projection.channels;
        auto const target = std::ranges::find(channels.target_channels, query.channel);
        std::optional<ChannelConversionPlan> conversion;
        if (target != channels.target_channels.end() && channels.transform_coefficients.empty())
            conversion = ChannelConversionRegistry::plan({channels.source_type, SampleStreamLayout::interleaved},
                {channels.target_type, SampleStreamLayout::interleaved});
        std::vector<Sample> source_values(projection.sources.size());
        std::array<Sample, maximum_supported_channel_count> target_values{};
        auto sample = [&](GlobalIndex position, std::span<std::size_t const> selected_regions) -> std::optional<Sample> {
            if (target == channels.target_channels.end()) return Sample{};
            for (std::size_t i = 0; i < projection.sources.size(); ++i) {
                auto const& term = projection.sources[i];
                auto const* view = source_views[i][selected_regions[source_groups[i]]];
                auto const* bytes = view ? view->value_storage(position.retreated_by(term.read_latency), term.channel) : nullptr;
                if (!bytes) return std::nullopt;
                std::memcpy(&source_values[i], bytes, sizeof(Sample));
            }
            auto const target_index = static_cast<std::size_t>(target - channels.target_channels.begin());
            if (conversion) {
                conversion->convert(source_values.data(), target_values.data(), 1);
                return target_values[target_index];
            }
            Sample value{};
            for (std::size_t i = 0; i < source_values.size(); ++i)
                value += source_values[i] * channels.transform_coefficients[target_index * source_values.size() + i];
            return value;
        };
        std::vector<SampleRegionProjectionSource<GlobalIndexRange>> sources;
        sources.reserve(region_sources.size());
        for (auto const& source : region_sources) sources.push_back({source.ranges, source.latency});
        visit_sample_region_projections<GlobalIndexRange>(
            sources, [&](auto range, std::span<std::size_t const> selected_regions) {
                if (query.canceled.load(std::memory_order_relaxed)) return false;
                if (query.result.regions.size() == maximum_observation_window_values) {
                    query.result.truncated = true;
                    return false;
                }
                auto& result = query.result.regions.emplace_back();
                result.range = range;
                result.region_key = {projection_index};
                result.region_key.insert(result.region_key.end(), selected_regions.begin(), selected_regions.end());
                for (auto const position : positions) {
                    if (!range.contains(position)) continue;
                    if (value_count++ == maximum_observation_window_values) {
                        query.result.truncated = true;
                        return false;
                    }
                    ObservationCaptureState captured;
                    if (auto const* cached = reuse ? reuse->value(result.region_key, position) : nullptr) {
                        result.values.emplace_back(position, *cached);
                        continue;
                    }
                    if (auto const value = sample(position, selected_regions))
                        query.operation->capture_point(captured, &*value);
                    result.values.emplace_back(position, captured);
                }
                return true;
            }, std::identity{}, window);
        if (query.result.truncated) break;
    }
    if (input.ordering == CoverageOrdering::sorted) {
        std::stable_sort(query.result.regions.begin(), query.result.regions.end(), [](auto const& left, auto const& right) {
            if (left.range.begin == right.range.begin) return left.range.extent > right.range.extent;
            return SignedGlobalIndexOrder{}(left.range.begin, right.range.begin);
        });
    }
}
} // namespace iv
