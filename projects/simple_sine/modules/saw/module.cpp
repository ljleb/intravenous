#include "intravenous/channel_ports.h"
#include "intravenous/lane_node/channels.h"
#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/node/layout.h>
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

    struct State
    {
        Sample s;
    };

    void initialize(InitializationContext<FunNode> const& ctx) const
    {
        ctx.state().s = 1.0f;
    }

    void tick_block(TickBlockContext<FunNode> const& ctx) const
    {
        auto& s = ctx.state().s;
        for (size_t i = 0; i < ctx.block_size; ++i)
        {
            ctx.outputs[0].push(s);
            s = s * 0.9999;
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
        auto const voice = g.node<SawOscillator>();

        NodeRef p;
        if constexpr (c == channels::stereo_left)
        {
            p = f + 2.5;
        }
        else
        {
            p = f - 2.5;
        }

        voice(
            "frequency"_P = p,
            "phase_offset"_P = phase,
            "dt"_P = dt);

        return c = voice * 0.1 * tt;
    });

    g.multi_channel<ChannelTypeId::stereo>([&] <auto c>
    {
        g.outputs(c = saw[c] + saw[iv::swap_side(c)]);
    });
}
}

IV_EXPORT_MODULE("iv.project.simple_sine", simple_sine);
