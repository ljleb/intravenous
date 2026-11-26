#pragma once

#include <intravenous/node/lifecycle.h>
#include <intravenous/node/message.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace iv {

struct SetBeatGrid {
    double bpm;
    std::uint32_t beats_per_bar;
    std::vector<std::uint32_t> subdivisions;
};

class Beats {
    using WideIndex = __int128_t;

    static constexpr std::uint32_t bpm_scale = 1'000'000;
    static constexpr std::uint32_t max_subdivisions_per_beat = 1024;
    static constexpr std::size_t max_subdivision_levels =
        std::bit_width(max_subdivisions_per_beat) - 1;
    static constexpr auto half_timeline = GlobalIndex::extent_type{1} << 63;
    static constexpr std::array regions{
        GlobalIndexRange{GlobalIndex::from_bits(half_timeline), half_timeline},
        GlobalIndexRange{GlobalIndex{}, half_timeline},
    };

    std::uint32_t scaled_bpm_;
    std::uint32_t beats_per_bar_;
    std::uint32_t subdivisions_per_beat_ = 1;
    std::array<std::uint32_t, max_subdivision_levels> subdivision_strides_{};

    struct SamplePeriod {
        WideIndex numerator;
        WideIndex denominator;
    };

    static std::uint32_t scale_tempo(double bpm)
    {
        auto const scaled = std::round(static_cast<long double>(bpm) * bpm_scale);
        if (!(scaled >= 1 && scaled <= std::numeric_limits<std::uint32_t>::max())) {
            throw std::invalid_argument("beats BPM is outside the supported range");
        }
        return static_cast<std::uint32_t>(scaled);
    }

    [[nodiscard]] SamplePeriod sample_period(std::size_t sample_rate) const
    {
        SamplePeriod const period{
            .numerator = WideIndex{sample_rate} * (60 * bpm_scale),
            .denominator = WideIndex{scaled_bpm_} * subdivisions_per_beat_,
        };
        if (period.numerator < period.denominator) {
            throw std::invalid_argument("beats subdivisions are closer than one project sample");
        }
        return period;
    }

    static WideIndex floor_divide(WideIndex value, WideIndex divisor) noexcept
    {
        return value / divisor - (value % divisor < 0);
    }

    static WideIndex ceil_divide(WideIndex value, WideIndex divisor) noexcept
    {
        return value / divisor + (value % divisor > 0);
    }

    struct BeatSequence {
        WideIndex stride;
        WideIndex exclude_every;

        [[nodiscard]] WideIndex index_at_or_after(WideIndex sample, SamplePeriod period) const noexcept
        {
            auto const mark = ceil_divide(sample * period.denominator - period.denominator / 2, period.numerator);
            auto const step = ceil_divide(mark, stride);
            return exclude_every ? step - ceil_divide(step, exclude_every) : step;
        }

        [[nodiscard]] GlobalIndex position(WideIndex index, SamplePeriod period) const noexcept
        {
            auto step = index;
            if (exclude_every) {
                auto const block = floor_divide(index, exclude_every - 1);
                step = block * exclude_every + (index - block * (exclude_every - 1)) + 1;
            }
            auto const mark = step * stride;
            auto const sample = floor_divide(mark * period.numerator + period.denominator / 2, period.denominator);
            return GlobalIndex::from_signed(static_cast<std::int64_t>(sample));
        }
    };

    [[nodiscard]] BeatSequence sequence(std::size_t depth) const noexcept
    {
        if (depth == 0) return {WideIndex{beats_per_bar_} * subdivisions_per_beat_, 0};
        if (depth == 1) return {subdivisions_per_beat_, beats_per_bar_};
        auto const stride = subdivision_strides_[depth - 2];
        auto const parent = depth == 2 ? subdivisions_per_beat_ : subdivision_strides_[depth - 3];
        return {stride, parent / stride};
    }

public:
    explicit Beats(double bpm = 120.0, std::uint32_t beats_per_bar = 4,
        std::vector<std::uint32_t> subdivisions = {2, 2})
        : scaled_bpm_(scale_tempo(bpm)), beats_per_bar_(beats_per_bar)
    {
        if (beats_per_bar_ == 0) {
            throw std::invalid_argument("beats per bar must be positive");
        }
        for (auto const factor : subdivisions) {
            if (factor < 2 || subdivisions_per_beat_ > max_subdivisions_per_beat / factor) {
                throw std::invalid_argument("beats subdivision factors must be at least two and multiply to at most 1024");
            }
            subdivisions_per_beat_ *= factor;
        }
        auto stride = subdivisions_per_beat_;
        for (std::size_t level = 0; level < subdivisions.size(); ++level) {
            stride /= subdivisions[level];
            subdivision_strides_[level] = stride;
        }
    }

    static constexpr auto outputs()
    {
        // Any requested sample can contain a mark, including when a query
        // selects many disjoint one-sample windows.
        return std::array{event_output({"beats", "Beats"}, EventTypeId::beat,
            OutputProduction::coverage, OutputRealization::computed,
            {.max_events_per_index = 1.0})};
    }

    void on_message(NodeMessageContext<Beats>& ctx, SetBeatGrid const& event) const
    {
        ctx.replace<"builtin.beats">(event.bpm, event.beats_per_bar, event.subdivisions);
    }

    void constrain_ports(ConstrainPortsContext<Beats>& ctx) const
    {
        ctx.output<"beats">().ordering() = CoverageOrdering::sorted;
    }

    void propagate_forward_coverage(PropagateForwardCoverageContext<Beats>& ctx) const
    {
        auto const period = sample_period(ctx.sample_rate);
        auto const depths = 2 + static_cast<std::size_t>(
            std::ranges::find(subdivision_strides_, 0u) - subdivision_strides_.begin());
        std::vector<EventRegionInfo> published;
        published.reserve(regions.size() * depths);
        for (auto const range : regions) {
            auto const begin = WideIndex{range.begin.signed_value()};
            for (std::size_t depth = 0; depth < depths; ++depth) {
                auto const beats = sequence(depth);
                auto const count = beats.index_at_or_after(begin + range.extent, period)
                    - beats.index_at_or_after(begin, period);
                published.push_back({range, IndexedEventRegionInfo{
                    static_cast<std::uint64_t>(count), BeatEvent{static_cast<std::uint8_t>(depth)}}});
            }
        }
        auto const output = ctx.output<"beats">();
        output.publish(published);
        if (ctx.local_state_changed) {
            output.change(IndexCoverage{regions[0], regions[1]});
        }
    }

    void tock_coverage(TockCoverageContext<Beats>& ctx) const
    {
        auto const period = sample_period(ctx.sample_rate);
        auto const output = ctx.output<"beats">();
        for (auto run : output.prepared_runs()) {
            auto const depth = std::get<BeatEvent>(*run.constant_value()).depth;
            auto const beats = sequence(depth);
            auto const origin = beats.index_at_or_after(run.region_range().begin.signed_value(), period);
            for (std::size_t slot = 0; slot < run.size(); ++slot)
                run.write_time(slot, beats.position(origin + run.index(slot), period));
        }

        std::optional<GlobalIndex> previous_half;
        std::uint8_t depth = 0;
        for (auto const region : output.regions()) {
            if (previous_half == region.range().begin) ++depth;
            else depth = 0;
            previous_half = region.range().begin;
            auto const beats = sequence(depth);
            for (auto const window : IndexCoverage{region.range()} & output.requested_coverage()) {
                WideIndex const begin = window.begin.signed_value();
                auto const end = beats.index_at_or_after(begin + window.extent, period);
                for (auto event = beats.index_at_or_after(begin, period); event < end; ++event) {
                    region.write({
                        .time = beats.position(event, period),
                        .value = BeatEvent{depth},
                    });
                }
            }
        }
    }
};

} // namespace iv
