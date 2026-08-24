#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>

inline void simple_sink(iv::GraphBuilder& g)
{
    using namespace iv;
    SamplePortRef left;
    SamplePortRef right;

    auto make_channel = [&]<auto Ch>() {
        auto const phase = g.node<PhaseIntegrator>();
        auto const osc = g.node<SawOscillator>();
        constexpr Sample channel_offset =
            std::same_as<decltype(Ch), decltype(stereo::left)> ? 0.0f : 1.0f;

        phase(0.0);
        auto const tone = osc(
            "frequency"_P = 110.0 + 55.0 * channel_offset,
            "phase_offset"_P = phase
        ) * 0.1;

        if constexpr (Ch == stereo::left) {
            left = tone;
        } else {
            right = tone;
        }
    };
    make_channel.template operator()<stereo::left>();
    make_channel.template operator()<stereo::right>();

    g.outputs("main"_P = g.tile<stereo>(left, right));
}
