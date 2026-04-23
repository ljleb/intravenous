#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"

inline void benchmark_constant_project(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = g.node<ValueSource>(&context.sample_period());
    auto const phase = g.node<PhaseIntegrator>();
    auto const voice_builder = context.load_builder("iv.test.benchmark_constant_voice");

    phase(0.0);
    auto const voice = g.embed_subgraph(voice_builder);
    auto const tone = voice(
        "amplitude"_P = 0.1,
        "frequency"_P = 110.0,
        "phase_offset"_P = phase,
        "dt"_P = dt
    );

    for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
        io.sink(g, channel)(tone);
    }

    g.outputs();
}

IV_EXPORT_MODULE("iv.test.benchmark_constant_project", benchmark_constant_project);
