#include "intravenous/channel_ports.h"
#include "intravenous/channel_layout.h"
#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/node/layout.h>
#include <intravenous/ports.h>

using namespace iv;

consteval auto pan(auto&& g, auto&& v, double t)
{
    auto const theta = (t + 1) * 0.25 * std::numbers::pi;
    auto const l_gain = std::cos(theta);
    auto const r_gain = std::sin(theta);

    return g.template tile<stereo>(v * l_gain, v * r_gain);
}

consteval void module_main(iv::GraphBuilder& g)
{
    auto const iters = 16;
    auto const f = g.input<"freq">(220, 0, 1000);
    auto const detune = g.input<"detune">(2.5, 0, 100);
    auto const detune_pair = detune * 2 / (iters - 1);
    auto const detune_osc = g.node<SineOscillator>();

    detune_osc("frequency"_P = 0.5 * detune_pair);

    for (size_t i = 0; i < iters; ++i)
    {
        auto const osc = g.node<SineOscillator>();
        osc("frequency"_P = f + i*detune_pair - detune);
        auto const result = pan(g, detune_osc * osc, t);
        g.outputs(result * 0.1);
    }
}
