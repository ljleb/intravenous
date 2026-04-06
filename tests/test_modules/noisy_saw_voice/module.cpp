#include "module/module.h"
#include "basic_nodes/shaping.h"
#include "basic_nodes/predictors.h"
#include "basic_nodes/filters.h"
#include "juce_vst_wrapper.h"

inline void noisy_saw_voice(iv::ModuleContext const& ctx)
{
    using namespace iv;
    auto& g = ctx.builder();
    auto const amplitude = g.input("amplitude", 0.1);
    auto const f = g.input("frequency", 500);
    auto const voice_noise = g.input("noise");
    auto const dt = g.input("dt", 1);
    auto const feedback = g.input("feedback", 1);

    // auto const predictor = g.node<NlmsPredictor>(4, 32);
    // auto const filter = g.node<SimpleIirLowPass>();
    // filter("cutoff"_P = 0.75, "dt"_P = dt);

    auto const integrator = g.node<Integrator>();
    auto const scc_latency = 1;
    auto const warper = g.node<Warper>();

    // integrator(filter(predictor(feedback)), frequency * 2, dt);
    integrator(feedback, f * 2, dt);
    // warper(feedback + f * dt * 2 + f * dt * (scc_latency-1) + voice_noise);
    warper(integrator + voice_noise);
    g.outputs(
        "out"_P = warper["anti_aliased"] * amplitude,
        "feedback"_P = warper["aliased"].detach(scc_latency)
    );
}

IV_EXPORT_MODULE("iv.test.noisy_saw_voice", noisy_saw_voice);
