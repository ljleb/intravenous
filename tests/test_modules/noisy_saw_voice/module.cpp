#include "module/module.h"
#include "basic_nodes/shaping.h"

namespace {
    inline void noisy_saw_voice(iv::ModuleContext const& ctx)
    {
        using namespace iv;
        auto& g = ctx.builder();
        auto const amplitude = g.input("amplitude", 0.1);
        auto const frequency = g.input("frequency", 500.0);
        auto const voice_noise = g.input("noise");
        auto const dt = g.input("dt", 1.0);
        auto const reset = g.input("reset", 1.0);

        auto const integrator = g.node<Integrator>();
        auto const warper = g.node<Warper>();

        integrator(warper["aliased"].detach() * reset, frequency * 2.0, dt);
        warper(integrator + voice_noise);
        g.outputs(warper["anti_aliased"] * amplitude);
    }
}

IV_EXPORT_MODULE("iv.test.noisy_saw_voice", noisy_saw_voice);
