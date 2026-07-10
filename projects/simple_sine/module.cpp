#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>

namespace
{
void simple_sine(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();

    auto const dt = g.node<ValueSource>(&context.sample_period());
    auto const phase = g.node<PhaseIntegrator>();
    auto const voice = g.node<SawOscillator>();

    voice(
        "frequency"_P = 440.0,
        "phase_offset"_P = phase,
        "dt"_P = dt);

    g.outputs(channels::mono = voice);
}
}

IV_EXPORT_MODULE("iv.project.simple_sine", simple_sine);
