#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"

inline void nested_loader_project(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = g.node<ValueSource>(&context.sample_period());

    auto const voice_builder = context.load_builder("iv.test.nested_loader_voice");
    SamplePortRef first_output;

    for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
        auto const sink = io.sink(g, channel);
        auto const phase = g.node<PhaseIntegrator>();
        auto const voice = g.embed_subgraph(voice_builder);

        phase(0.0);
        auto tone = voice(
            "amplitude"_P = 0.25,
            "frequency"_P = 220.0 + 110.0 * static_cast<Sample>(channel),
            "phase_offset"_P = phase,
            "dt"_P = dt
        );

        if (channel == 0) {
            first_output = tone;
        }
        sink(channel == 0 ? tone : first_output);
    }

    g.outputs();
}

IV_EXPORT_MODULE("iv.test.nested_loader_project", nested_loader_project);
