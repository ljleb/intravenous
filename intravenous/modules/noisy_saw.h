#pragma once

#include "modules/module.h"

namespace iv::modules {
    inline TypeErasedNode noisy_saw()
    {
        GraphBuilder g;

        auto const amplitude = g.input("amplitude", 0.5);
        auto const frequency = g.input("frequency", 500.0);
        auto const voice_noise = g.input("noise");
        auto const dt = g.input("dt", 1.0);
        auto const reset = g.input("reset", 1.0);

        auto const integrator = g.node<Integrator>();
        auto const warper = g.node<Warper>();
        auto const latency = g.node<Latency>(0);

        integrator(warper["aliased"].detach() * reset, frequency * 2.0, dt);
        warper(latency(integrator) + voice_noise);
        g.outputs(warper["anti_aliased"] * amplitude);

        return TypeErasedNode(std::move(g).build());
    }
}
