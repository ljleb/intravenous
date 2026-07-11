#include "intravenous/channel_ports.h"
#include "intravenous/lane_node/channels.h"
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

    auto saw = g.multi_channel<ChannelTypeId::stereo>([&]<auto c>()
    {
        auto f = g.node<Constant>(440);
        auto d = g.node<Constant>(2);
        auto const voice = g.node<SawOscillator>();

        NodeRef p;
        if constexpr (c == channels::stereo_left)
        {
            p = f + d;
        }
        else
        {
            p = f - d;
        }

        voice(
            "frequency"_P = p,
            "phase_offset"_P = phase,
            "dt"_P = dt);

        return c = voice * 0.1;
    });

    g.multi_channel<ChannelTypeId::stereo>([&] <auto c>
    {
        g.outputs(c = saw[c]);
    });
}
}

IV_EXPORT_MODULE("iv.project.simple_sine", simple_sine);
