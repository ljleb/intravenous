#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"

void polyphonic_module(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = g.node<ValueSource>(&context.sample_period());
    auto const sink = io.sink(g, 0);
    auto voice = iv::polyphonic<2>(g, [&](auto m) {
        auto const saw = g.node<SawOscillator>();
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0,
            "dt"_P = dt
        );
        return saw * ("amplitude"_P << m);
    });
    auto x = std::move(voice);
    sink(x);
    g.outputs();
}

IV_EXPORT_MODULE("iv.test.polyphonic_module", polyphonic_module);
