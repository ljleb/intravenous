#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>

void reload_project(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const left_phase = g.node<"phase_integrator">();
    auto const right_phase = g.node<"phase_integrator">();
    auto const left_voice = g.node<"iv.test.reload_voice">();
    auto const right_voice = g.node<"iv.test.reload_voice">();
    left_phase(0.0);
    right_phase(0.0);
    g.outputs(
        "main"_P.with_display_name("Main")[stereo::left] = left_voice(
            "amplitude"_P.with_display_name("Amplitude") = 0.25,
            "frequency"_P.with_display_name("Frequency") = 220.0,
            "phase_offset"_P.with_display_name("Phase offset") = left_phase),
        "main"_P.with_display_name("Main")[stereo::right] = right_voice(
            "amplitude"_P.with_display_name("Amplitude") = 0.25,
            "frequency"_P.with_display_name("Frequency") = 330.0,
            "phase_offset"_P.with_display_name("Phase offset") = right_phase));
}

IV_MODULE("iv.test.reload_project", reload_project, "Reload project");
