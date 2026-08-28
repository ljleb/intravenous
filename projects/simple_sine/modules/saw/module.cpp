#include "intravenous/channel_ports.h"
#include "intravenous/channel_layout.h"
#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/node/layout.h>
#include <intravenous/ports.h>
#include <ranges>

using namespace iv;

consteval auto pan(auto&& g, double t)
{
    auto const theta = (t + 1) * 0.25 * std::numbers::pi;
    auto const l_gain = std::cos(theta);
    auto const r_gain = std::sin(theta);

    return g.template tile<stereo>(l_gain, r_gain);
}

consteval void module_main(iv::GraphBuilder& g)
{
    constexpr size_t iters = 16;
    auto const f = g.input<"freq">(220, 0, 1000);
    auto const detune = g.input<"detune">(2.5, 0, 100);
    auto const detune_pair = detune*2 / (iters-1);

    template for (constexpr auto i : std::views::iota(size_t{0}, size_t{iters + 1}))
    {
        auto const add_voice = [&] (auto&& gain) {
            auto const osc = g.template node<SineOscillator>();
            auto const t = (static_cast<double>(i) - 0.5) * detune_pair - detune;
            osc("frequency"_P = f + t, "phase_offset"_P = 0.25); // cosine
            g.outputs(osc * gain * 0.1);
        };

        if constexpr (i == 0) {
            add_voice(0.5 * pan(g, -1.0));
        }
        else if constexpr (i == iters) {
            add_voice(-0.5 * pan(g, 1.0));
        }
        else {
            constexpr auto t0 = (i - 1) * 2.0 / (iters - 1) - 1.0;
            constexpr auto t1 = i * 2.0 / (iters - 1) - 1.0;
            add_voice(0.5*(pan(g, t1) - pan(g, t0)));
        }
    }
}
