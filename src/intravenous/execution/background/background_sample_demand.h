#pragma once

#include <intravenous/coverage.h>
#include <intravenous/sample_positions.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace iv {

// A union of sample positions. Each piece stays compact when it is clipped
// against logical coverage or shifted along a same-pace route.
class BackgroundSampleDemand {
    std::vector<SamplePositionRun> runs_;
    std::vector<GlobalIndex> positions_;

    static bool contains(SamplePositionRun const& outer, SamplePositionRun const& inner)
    {
        auto const first = outer.slot_at(inner.first);
        if (!first) return false;
        if (inner.count == 1) return true;
        if (inner.step % outer.step) return false;
        return (inner.count - 1) <= (outer.count - 1 - *first) / (inner.step / outer.step);
    }

public:
    [[nodiscard]] bool empty() const noexcept { return runs_.empty() && positions_.empty(); }
    [[nodiscard]] SamplePositionsView view() const noexcept { return {runs_, positions_}; }
    bool operator==(BackgroundSampleDemand const&) const = default;

    [[nodiscard]] bool intersects(GlobalIndexRange range) const
    {
        for (auto const run : runs_) {
            bool found = false;
            for_each_sample_position_intersection(run, range, [&](SamplePositionRun) { found = true; });
            if (found) return true;
        }
        return std::ranges::any_of(positions_, [&](auto position) { return range.contains(position); });
    }

    [[nodiscard]] bool contains(GlobalIndex position) const noexcept
    {
        return std::ranges::contains(positions_, position)
            || std::ranges::any_of(runs_, [&](auto const& run) { return run.slot_at(position).has_value(); });
    }

    // Preserve regular pieces. Only overlapping intervals need their exact
    // contents merged to guarantee one slot per position.
    [[nodiscard]] std::vector<SamplePositionRun> prepared_runs(
        GlobalIndexRange region, IndexCoverage const& dense, std::size_t frame_budget) const
    {
        if (dense.contains(region)) return {{region.begin, region.extent}};
        auto selected = intersected(IndexCoverage{region});
        selected.include(dense & IndexCoverage{region});
        auto runs = std::move(selected.runs_);
        for (auto position : selected.positions_) runs.push_back({position, 1});
        auto const offset = [&](GlobalIndex position) { return region.begin.forward_distance_to(position); };
        std::ranges::sort(runs, {}, [&](auto const& run) { return offset(run.first); });
        std::vector<SamplePositionRun> result;
        for (std::size_t first = 0; first < runs.size();) {
            auto end = first + 1;
            auto last = offset(runs[first].position(runs[first].count - 1));
            while (end < runs.size() && offset(runs[end].first) <= last) {
                last = std::max(last, offset(runs[end].position(runs[end].count - 1)));
                ++end;
            }
            if (end == first + 1) {
                result.push_back(runs[first]);
            } else {
                std::vector<GlobalIndex> positions;
                for (auto i = first; i < end; ++i) {
                    auto const run = runs[i];
                    if (run.count > frame_budget - positions.size())
                        throw std::length_error("overlapping sample demand exceeds its work budget");
                    for (std::uint64_t slot = 0; slot < run.count; ++slot) positions.push_back(run.position(slot));
                }
                std::ranges::sort(positions, {}, offset);
                positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
                for (std::size_t begin = 0; begin < positions.size();) {
                    auto next = begin + 1;
                    auto const step = next == positions.size() ? 1 : positions[begin].forward_distance_to(positions[next]);
                    while (next < positions.size() && positions[next - 1].forward_distance_to(positions[next]) == step) ++next;
                    result.push_back({positions[begin], next - begin, step});
                    begin = next;
                }
            }
            first = end;
        }
        return result;
    }

    [[nodiscard]] IndexCoverage positional_ranges(std::size_t max_sparse_positions) const
    {
        IndexCoverage result;
        for (auto const run : runs_) {
            if (run.step == 1) {
                result.include({run.first, run.count});
                continue;
            }
            if (run.count > max_sparse_positions) throw std::length_error("background sample demand exceeds its work budget");
            max_sparse_positions -= static_cast<std::size_t>(run.count);
            for (std::uint64_t slot = 0; slot < run.count; ++slot) result.include({run.position(slot), 1});
        }
        if (positions_.size() > max_sparse_positions) throw std::length_error("background sample demand exceeds its work budget");
        for (auto const position : positions_) result.include({position, 1});
        return result;
    }

    bool include(SamplePositionRun run)
    {
        if (!run.valid()) throw std::invalid_argument("invalid background sample position run");
        if (!run.count) return false;
        if (std::ranges::any_of(runs_, [&](auto const& existing) { return contains(existing, run); })) return false;
        std::erase_if(runs_, [&](auto const& existing) { return contains(run, existing); });
        std::erase_if(positions_, [&](auto position) { return run.slot_at(position).has_value(); });
        runs_.push_back(run);
        return true;
    }

    bool include(GlobalIndex position)
    {
        if (std::ranges::contains(positions_, position)
            || std::ranges::any_of(runs_, [&](auto const& run) { return run.slot_at(position).has_value(); }))
            return false;
        positions_.push_back(position);
        return true;
    }

    bool include(SamplePositionsView demand)
    {
        bool grew = false;
        for (auto const run : demand.runs) grew |= include(run);
        for (auto const position : demand.individual_positions) grew |= include(position);
        return grew;
    }

    bool include(IndexCoverage const& coverage)
    {
        bool grew = false;
        for (auto const range : coverage)
            grew |= include(SamplePositionRun{range.begin, range.extent});
        return grew;
    }

    void include_ordered_positions(std::span<GlobalIndex const> positions)
    {
        while (!positions.empty()) {
            if (positions.size() == 1) {
                include(positions.front());
                return;
            }
            auto const step = positions[0].forward_distance_to(positions[1]);
            if (step == 0) {
                include(positions.front());
                positions = positions.subspan(1);
                continue;
            }
            std::size_t count = 2;
            while (count < positions.size()
                   && positions[count - 1].forward_distance_to(positions[count]) == step
                   && count <= std::numeric_limits<std::uint64_t>::max() / step) ++count;
            include(SamplePositionRun{positions.front(), count, step});
            positions = positions.subspan(count);
        }
    }

    [[nodiscard]] BackgroundSampleDemand intersected(IndexCoverage const& coverage) const
    {
        BackgroundSampleDemand restricted;
        for (auto const range : coverage) {
            for (auto const run : runs_)
                for_each_sample_position_intersection(run, range,
                    [&](SamplePositionRun part) { restricted.include(part); });
        }
        for (auto const position : positions_)
            if (coverage.contains(position)) restricted.include(position);
        return restricted;
    }

    [[nodiscard]] BackgroundSampleDemand retreated_by(std::uint64_t latency) const
    {
        auto shifted = *this;
        for (auto& run : shifted.runs_) run.first = run.first.retreated_by(latency);
        for (auto& position : shifted.positions_) position = position.retreated_by(latency);
        return shifted;
    }
};

struct SampleDemandByRegion {
    std::vector<std::pair<std::size_t, BackgroundSampleDemand>> values;

    [[nodiscard]] BackgroundSampleDemand const* find(std::size_t index) const noexcept
    {
        auto const found = std::ranges::find(values, index, &std::pair<std::size_t, BackgroundSampleDemand>::first);
        return found == values.end() ? nullptr : &found->second;
    }

    bool include(std::size_t index, SamplePositionsView positions)
    {
        auto found = std::ranges::find(values, index, &std::pair<std::size_t, BackgroundSampleDemand>::first);
        if (found == values.end()) {
            values.emplace_back(index, BackgroundSampleDemand{});
            found = std::prev(values.end());
        }
        return found->second.include(positions);
    }

    [[nodiscard]] BackgroundSampleDemand including(
        std::size_t index, BackgroundSampleDemand const& positional) const
    {
        auto result = positional;
        if (auto const* local = find(index)) result.include(local->view());
        return result;
    }
};

} // namespace iv
