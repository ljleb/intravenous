#include <intravenous/node/lifecycle.h>
#include <intravenous/node/config_relocations.h>
#include <intravenous/node/config_string.h>

#include <array>
#include <cstddef>
#include <iostream>

struct DebugProbe {
    iv::NodeConfigString label = "debug";
    size_t every_n_ticks = 4800;

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 1>{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{};
    }

    void collect_config_string_relocations(iv::NodeConfigStringRelocations& result) const
    {
        result.push_back({
            offsetof(DebugProbe, label) + offsetof(iv::NodeConfigString, data),
            std::string(label.view()),
        });
    }

    void tick(iv::TickSampleContext<DebugProbe> const& ctx) const
    {
        auto const sample = ctx.inputs[0].get();
        if (every_n_ticks != 0 && (ctx.index % every_n_ticks) == 0) {
            if (label.size != 0) {
                std::cout.write(label.data, static_cast<std::streamsize>(label.size));
            }
            std::cout << "[" << ctx.index << "] = " << sample << '\n';
        }
        ctx.outputs[0].push(sample);
    }
};
