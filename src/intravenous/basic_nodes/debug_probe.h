#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>
#include <cstddef>
#include <iostream>

struct DebugProbe {
    char const* label = "debug";
    size_t every_n_ticks = 4800;

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{};
    }

    void tick(iv::TickSampleContext<DebugProbe> const& ctx) const
    {
        auto const sample = ctx.inputs[0].get();
        if (every_n_ticks != 0 && (ctx.index % every_n_ticks) == 0) {
            std::cout << label;
            std::cout << "[" << ctx.index << "] = " << sample << '\n';
        }
        ctx.outputs[0].push(sample);
    }
};
