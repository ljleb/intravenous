#include "intravenous/channel_ports.h"
#include "intravenous/lane_node/channels.h"
#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/ports.h>

using namespace iv;

struct FunNode
{
    auto outputs() const
    {
        return std::array<OutputConfig, 1>
        {
            OutputConfig { .name = "out" },
        };
    }

    void tick_block(TickBlockContext<FunNode> const& ctx) const
    {
        Sample v = std::pow(1.0001, -static_cast<Sample>(ctx.index));
        for (size_t i = 0; i < ctx.block_size; ++i)
        {
            ctx.outputs[0].push(v);
        }
    }
};

namespace
{
void simple_sine(iv::ModuleContext const& context)
{
    auto& g = context.builder();

    auto const dt = g.node<ValueSource>(&context.sample_period());
    auto const phase = g.node<PhaseIntegrator>();
    auto const tt = g.node<FunNode>();

    auto saw = g.multi_channel<ChannelTypeId::stereo>([&]<auto c>()
    {
        auto f = g.node<Constant>(220);
        auto d = g.input(2, 0);
        auto const voice = g.node<SawOscillator>();

        NodeRef p;
        if constexpr (c == channels::stereo_left)
        {
            p = f + d/2;
        }
        else
        {
            p = f - d/2;
        }

        voice(
            "frequency"_P = p,
            "phase_offset"_P = phase,
            "dt"_P = dt);
        
        return c = voice * 0.1 * tt;
    });

    g.multi_channel<ChannelTypeId::stereo>([&] <auto c>
    {
        g.outputs(c = saw[c], iv::swap_side(c) = saw[c]);
    });
}
}

IV_EXPORT_MODULE("iv.project.simple_sine", simple_sine);
