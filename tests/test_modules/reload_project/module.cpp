#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>

inline void reload_project(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const dt = g.node<ValueSource>(&context.sample_period());

    auto const voice_builder = context.load_builder("iv.test.reload_voice");
    auto const left_phase = g.node<PhaseIntegrator>();
    auto const right_phase = g.node<PhaseIntegrator>();
    auto const left_voice = g.embed_subgraph(voice_builder);
    auto const right_voice = g.embed_subgraph(voice_builder);

    left_phase(0.0);
    right_phase(0.0);
    g.outputs(
        channels::stereo_left = left_voice(
            "amplitude"_P = 0.25,
            "frequency"_P = 220.0,
            "phase_offset"_P = left_phase,
            "dt"_P = dt
        ),
        channels::stereo_right = right_voice(
            "amplitude"_P = 0.25,
            "frequency"_P = 330.0,
            "phase_offset"_P = right_phase,
            "dt"_P = dt
        )
    );
}

IV_EXPORT_MODULE("iv.test.reload_project", reload_project);
