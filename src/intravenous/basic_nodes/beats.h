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

    [[nodiscard]] BeatEvent beat_at(WideIndex mark_index) const noexcept
    {
        if (mark_index % subdivisions_per_beat_ == 0) {
            auto const beat_index = mark_index / subdivisions_per_beat_;
            return {.depth = static_cast<std::uint8_t>(beat_index % beats_per_bar_ != 0)};
        }
        std::size_t level = 0;
        while (mark_index % subdivision_strides_[level] != 0) ++level;
        return {.depth = static_cast<std::uint8_t>(level + 2)};
    }

    static WideIndex floor_divide(WideIndex value, WideIndex divisor) noexcept
    {
        return value / divisor - (value % divisor < 0);
    }

    static WideIndex ceil_divide(WideIndex value, WideIndex divisor) noexcept
    {
        return value / divisor + (value % divisor > 0);
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
        return std::array{event_output("beats", EventTypeId::beat,
            OutputProduction::coverage, OutputRealization::computed,
            {.max_events_per_index = 1.0})};
    }

    void on_message(NodeMessageContext<Beats>& ctx, SetBeatGrid const& event) const
    {
        ctx.replace<"iv.builtin.beats">(event.bpm, event.beats_per_bar, event.subdivisions);
    }

    void constrain_ports(ConstrainPortsContext<Beats>& ctx) const
    {
        ctx.output<"beats">().ordering() = CoverageOrdering::sorted;
    }

    void propagate_forward_coverage(PropagateForwardCoverageContext<Beats>& ctx) const
    {
        (void)sample_period(ctx.sample_rate);
        auto const output = ctx.output<"beats">();
        output.publish(regions);
        if (ctx.local_state_changed) {
            output.change(IndexCoverage{regions[0], regions[1]});
        }
    }

    void tock_coverage(TockCoverageContext<Beats>& ctx) const
    {
        auto const period = sample_period(ctx.sample_rate);
        auto const output = ctx.output<"beats">();
        auto const rounding = period.denominator / 2;
        // Invert nearest-sample rounding to find the first mark in the window.
        // Derive each position from its mark index so partitioning cannot drift.
        auto const first_mark_at_or_after = [&](WideIndex sample) {
            return ceil_divide(sample * period.denominator - rounding,
                period.numerator);
        };
        for (auto const region : output.regions()) {
            for (auto const window : IndexCoverage{region.range()} & output.requested_coverage()) {
                WideIndex const begin = window.begin.signed_value();
                auto const end = first_mark_at_or_after(begin + window.extent);
                for (auto mark = first_mark_at_or_after(begin); mark < end; ++mark) {
                    auto const sample = floor_divide(
                        mark * period.numerator + rounding, period.denominator);
                    region.write({
                        .time = GlobalIndex::from_signed(static_cast<std::int64_t>(sample)),
                        .value = beat_at(mark),
                    });
                }
            }
        }
    }
};

} // namespace iv
