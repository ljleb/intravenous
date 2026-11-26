#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <tuple>

namespace iv {

struct TransportPosition {
    static constexpr auto replay_mode = TickReplayMode::enabled;

    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{output<FractionalGlobalIndex>(
            {"position", "Position"}, OutputProduction::sequential)};
    }

    void tick(TickSampleContext<TransportPosition> const& ctx) const
    {
        ctx.output<"position">().push(ctx.index);
    }
};

struct LinearResampler {
    static constexpr auto replay_mode = TickReplayMode::enabled;
    static constexpr auto layout = mono_planar_channel_layout;

    static constexpr auto inputs()
    {
        return std::tuple{
            input<FractionalGlobalIndex>({"position", "Position"}, InputAccess::sequential),
            input<Sample>({"source", "Source"}, InputAccess::coverage, InputContentAccess::windowed),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{output<Sample>({"out", "Out"}, OutputProduction::sequential)};
    }

    void constrain_ports(ConstrainPortsContext<LinearResampler>& ctx) const
    {
        ctx.input<"position">().pace() = ctx.output<"out">().pace();
        auto const lead = ctx.input<"source">().preparation_lead();
        ctx.input<"position">().history() = lead;
        ctx.input<"source">().pace() = independent_pace;
        ctx.output<"out">().latency() = lead;
    }

    void tick(TickSampleContext<LinearResampler> const& ctx) const
    {
        auto const position = ctx.input<"position">();
        auto const source = ctx.input<"source">();
        auto const out = ctx.output<"out">();
        auto const lead = out.latency();
        auto const next = ctx.input_has_coverage<"position">()
            ? interpolate_global_lattice(position.get(), source.absolute_pace_exponent())
            : std::nullopt;
        if (lead == 0) {
            out() = next ? interpolate(source, *next) : Sample{};
            return;
        }

        std::array<GlobalIndex, 2> taps{};
        std::size_t tap_count = 0;
        if (next) {
            taps = {next->lower, next->upper};
            tap_count = next->fraction ? 2 : 1;
        }
        auto const request = ctx.request_input<"source">({
            .individual_positions = std::span{taps}.first(tap_count),
        });
        if (!request.due.empty() && !request.due.begin()->positions.empty()) {
            auto const due = interpolate_global_lattice(
                position.get(lead), source.absolute_pace_exponent());
            if (due) out.update(interpolate(request.input, *due), lead - 1);
        }
        out() = Sample{};
    }

private:
    static Sample interpolate(details::StaticBackgroundContinuousInputAccess<Sample, InputAccessKind::coverage> const& source,
        GlobalLatticeInterpolation taps)
    {
        auto sample_at = [&](GlobalIndex index) {
            auto const window = source.prepared_window_at(index);
            IV_ASSERT(window || !source.coverage().contains(index),
                "a covered resampler tap must be prepared before Tick");
            return window ? window->at(index, 0).value : 0.f;
        };
        auto const a = sample_at(taps.lower);
        if (taps.fraction == 0) return Sample{a};
        auto const b = sample_at(taps.upper);
        auto const weight = static_cast<double>(taps.fraction) * 0x1p-64;
        return Sample{static_cast<float>(std::lerp(static_cast<double>(a), static_cast<double>(b), weight))};
    }
};

} // namespace iv
