#include "module/module.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"

inline void simple_sink(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = g.node<ValueSource>(&context.sample_period());

    for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
        auto const sink = io.sink(g, channel);
        auto const phase = g.node<PhaseIntegrator>();
        auto const osc = g.node<SawOscillator>();

        phase(0.0);
        sink(osc(
            "frequency"_P = 110.0 + 55.0 * static_cast<Sample>(channel),
            "phase_offset"_P = phase,
            "dt"_P = dt
        ) * 0.1);
    }

    g.outputs();
}

IV_EXPORT_MODULE("iv.test.simple_sink", simple_sink);
