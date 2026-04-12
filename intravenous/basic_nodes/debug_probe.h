#include <node_lifecycle.h>

#include <array>
#include <iostream>
#include <string>

struct DebugProbe {
    std::string label = "debug";
    size_t every_n_ticks = 4800;

    auto inputs() const
    {
        return std::array<iv::InputConfig, 1>{};
    }

    auto outputs() const
    {
        return std::array<iv::OutputConfig, 1>{};
    }

    void tick(iv::TickSampleContext<DebugProbe> const& ctx) const
    {
        auto const sample = ctx.inputs[0].get();
        if (every_n_ticks != 0 && (ctx.index % every_n_ticks) == 0) {
            std::cout << label << "[" << ctx.index << "] = " << sample << '\n';
        }
        ctx.outputs[0].push(sample);
    }
};
