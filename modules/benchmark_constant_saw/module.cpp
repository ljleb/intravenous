#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"

inline void benchmark_constant_saw(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = NODE(g, ValueSource, &context.sample_period());

    auto const phase = NODE(g, PhaseIntegrator);
    auto const osc = NODE(g, SawOscillator);
    phase(0.0);
    auto const tone = osc(
        "frequency"_P = 110.0,
        "phase_offset"_P = phase,
        "dt"_P = dt
    ) * 0.1;

    for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
        io.sink(g, channel)(tone);
    }

    g.outputs();
}

IV_EXPORT_MODULE("iv.test.benchmark_constant_saw", benchmark_constant_saw);
