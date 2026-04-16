#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/filters.h"
#include "basic_nodes/shaping.h"

inline void benchmark_constant_filtered(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = NODE(g, ValueSource, &context.sample_period());

    auto const phase = NODE(g, PhaseIntegrator);
    auto const osc = NODE(g, SawOscillator);
    auto const low_pass = NODE(g, SimpleIirLowPass);
    auto const high_pass = NODE(g, SimpleIirHighPass);

    phase(0.0);
    osc(
        "frequency"_P = 110.0,
        "phase_offset"_P = phase,
        "dt"_P = dt
    );
    low_pass(osc, 0.35, dt);
    high_pass(low_pass, 0.08, dt);

    auto const tone = high_pass * 0.12;
    for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
        io.sink(g, channel)(tone);
    }

    g.outputs();
}

IV_EXPORT_MODULE("iv.test.benchmark_constant_filtered", benchmark_constant_filtered);
