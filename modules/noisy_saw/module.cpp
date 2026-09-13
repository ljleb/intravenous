#include <intravenous/dsl.h>
#include <intravenous/juce/vst_wrapper.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

void noisy_saw_project(iv::GraphBuilder& g)
{
    using namespace iv;

    // auto const sup = juce::vst(g, "ValhallaSupermassive");
    // sup(
    //     "Mix"_P = 0.2,
    //     "DelayWarp"_P = 1.0,
    //     "Density"_P = 1.0,
    //     "Mode"_P = 2 / 24.f
    // );

    size_t seed = 0;
    SamplePortRef left;
    SamplePortRef right;
    auto make_channel = [&]<auto Ch>() {
        auto const saw = g.node<"saw_oscillator">();
        auto const phi = g.node<"phase_integrator">();
        auto const generator = g.node<"deterministic_uniform_aes_noise">(
            std::optional<std::uint64_t>{static_cast<std::uint64_t>(seed++)});
        auto const u_to_n = g.node<"uniform_to_gaussian">(
            Sample{0.0f}, Sample{0.5f});
        auto const lo_pass = g.node<"simple_iir_low_pass">();
        auto const hi_pass = g.node<"simple_iir_high_pass">();
        auto const lp = g.node<"simple_iir_low_pass">();

        u_to_n(generator);
        lo_pass(u_to_n, 0.0);
        hi_pass(lo_pass, 0.3);
        phi(hi_pass * 0.1);
        saw(
            "frequency"_P = 110.0,
            "phase_offset"_P = phi
        );
        lp(saw * 0.1, 0.5);
        auto const channel_output = lp * 0.5;
        if constexpr (Ch == stereo::left) {
            left = channel_output;
        } else {
            right = channel_output;
        }
    };
    make_channel.template operator()<stereo::left>();
    make_channel.template operator()<stereo::right>();

    g.outputs("main"_P = g.tile<stereo>(left, right));
}

IV_MODULE("iv.test.noisy_saw", noisy_saw_project);
