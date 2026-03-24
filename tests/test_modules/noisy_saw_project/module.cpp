#include "module/module.h"
#include "basic_nodes/noise.h"
#include "basic_nodes/filters.h"
#include "basic_nodes/shaping.h"
#include "basic_nodes/buffers.h"

namespace iv::modules {
    inline void noise_voice(GraphBuilder& g)
    {
        auto const dt = g.input("dt", 1.0);

        auto const level_knob = 1.2;
        auto const lo_pass_knob = 1.0;
        auto const hi_pass_knob = 1.0;
        auto const generator = g.node<DeterministicUniformAESNoise>();
        auto const lo_pass = g.node<SimpleIirLowPass>();
        auto const hi_pass = g.node<SimpleIirHighPass>();

        auto const u_to_n_knob = 0.0;
        auto const u_to_n = g.node<UniformToGaussian>(0.0, 0.5);
        auto const u_to_c = g.node<UniformToCauchy>(0.0, 0.01);
        auto const interp = g.node<Interpolation>();

        generator({ {"min", -1}, {"max", 1} });

        u_to_c(generator);
        u_to_n(generator);

        interp(u_to_n, u_to_c, u_to_n_knob);

        lo_pass(interp, lo_pass_knob, dt);
        hi_pass(lo_pass, hi_pass_knob, dt);

        g.outputs(hi_pass * level_knob);
    }

    inline void noisy_saw_project(ModuleContext const& context)
    {
        GraphBuilder& g = context.builder();
        ModuleSystem const& system = context.system();
        TypeErasedModule voice_module = context.load("iv.test.noisy_saw_voice");

        auto const dt = g.node<ValueSource>(&system.sample_period());
        SignalRef first_noise;

        for (size_t channel = 0; channel < system.render_config().num_channels; ++channel) {
            auto const noise = g.subgraph(noise_voice);
            if (channel == 0) {
                first_noise = noise;
            }

            auto const voice = g.node<TypeErasedNode>(voice_module.build(context));
            auto const shared_noise = g.node<Interpolation>();
            auto const sink = system.sink(g, channel);

            noise(dt);
            shared_noise(first_noise, noise, 1.0);
            voice({ {"noise", shared_noise}, {"dt", dt} });
            sink(voice);
        }

        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.noisy_saw_project", iv::modules::noisy_saw_project);
