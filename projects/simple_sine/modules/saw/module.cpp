#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>

using namespace iv;

auto pan(auto&& g, double t)
{
    auto const theta = (t + 1) * 0.25 * std::numbers::pi;
    auto const l_gain = std::cos(theta);
    auto const r_gain = std::sin(theta);

    return g.template tile<stereo>(l_gain, r_gain);
}

void module_main(iv::GraphBuilder& g)
{
    constexpr size_t iters = 16;
    auto const f = g.input<"freq">(220, 0, 1000);
    auto const detune = g.input<"detune">(2.5, -100, 100);
    auto const detune_pair = detune*2 / (iters-1);

    for (size_t i = 0; i < iters + 1; ++i)
    {
        auto const add_voice = [&] (auto&& gain) {
            auto const osc = g.node<SineOscillator>();
            auto const t = (static_cast<double>(i) - 0.5) * detune_pair - detune;
            osc("frequency"_P = f + t, "phase_offset"_P = 0.25); // cosine
            g.outputs(osc * gain * 0.1);
        };

        if (i == 0) {
            add_voice(0.5 * pan(g, -1.0));
        }
        else if (i == iters) {
            add_voice(-0.5 * pan(g, 1.0));
        }
        else {
            auto t0 = (i - 1) * 2.0 / (iters - 1) - 1.0;
            auto t1 = i * 2.0 / (iters - 1) - 1.0;
            add_voice(0.5*(pan(g, t1) - pan(g, t0)));
        }
    }
}

IV_MODULE("iv.project.saw", module_main);
