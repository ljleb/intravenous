#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>

inline void nested_loader_project(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const dt = g.node<ValueSource>(&context.sample_period());

    auto const voice_builder = context.load_builder("iv.test.nested_loader_voice");
    auto const phase = g.node<PhaseIntegrator>();
    auto const voice = g.embed_subgraph(voice_builder);

    phase(0.0);
    auto const tone = voice(
        "amplitude"_P = 0.25,
        "frequency"_P = 220.0,
        "phase_offset"_P = phase,
        "dt"_P = dt
    );

    g.outputs(channels::stereo_left = tone, channels::stereo_right = tone);
}

IV_EXPORT_MODULE("iv.test.nested_loader_project", nested_loader_project);
