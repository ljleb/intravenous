#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>
#include <cstddef>

namespace iv::details {
    void write_debug_probe_sample(
        char const* label, GlobalIndex tick_index, Sample sample);
}

struct DebugProbe {
    char const* label = "debug";
    size_t every_n_ticks = 4800;

    constexpr DebugProbe(
        char const* label_ = "debug", size_t every_n_ticks_ = 4800)
        : label(label_)
        , every_n_ticks(every_n_ticks_)
    {}

    static constexpr auto inputs()
    {
        return std::array<iv::SequentialInputConfig<iv::Sample>, 1>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::TickOutputConfig<iv::Sample>, 1>{};
    }

    void tick(iv::TickSampleContext<DebugProbe> const& ctx) const
    {
        auto const sample = ctx.template input<0>().get();
        if (every_n_ticks != 0
            && (ctx.index.bits() % every_n_ticks) == 0) {
            iv::details::write_debug_probe_sample(label, ctx.index, sample);
        }
        ctx.template output<0>().push(sample);
    }
};
