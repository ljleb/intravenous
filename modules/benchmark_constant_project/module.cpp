#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>

inline void benchmark_constant_project(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
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

    g.outputs("main"_P[stereo::left] = tone, "main"_P[stereo::right] = tone);
}

IV_EXPORT_MODULE("iv.test.benchmark_constant_project", benchmark_constant_project);
