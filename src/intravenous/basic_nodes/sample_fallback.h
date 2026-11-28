#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>

namespace iv::details {

// The paired inputs retain independent coverage. The acyclic output is
// available wherever either source is available.
struct SampleFallback {
    static constexpr auto replay_mode = TickReplayMode::enabled;

    static constexpr auto inputs()
    {
        return std::array{
            input<Sample>({"primary", "Primary"}, InputAccess::sequential),
            input<Sample>({"fallback", "Fallback"}, InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{output<Sample>({"out", "Out"}, OutputProduction::sequential)};
    }

    void propagate_forward_coverage(PropagateForwardCoverageContext<SampleFallback>& ctx) const
    {
        ctx.output<"out">().publish(ctx.input<"primary">().coverage() | ctx.input<"fallback">().coverage());
    }

    void tick(TickSampleContext<SampleFallback> const& ctx) const
    {
        auto const value = ctx.input_has_coverage<"primary">()
            ? ctx.input<"primary">().get()
            : ctx.input<"fallback">().get();
        ctx.output<"out">().push(value);
    }
};

} // namespace iv::details
