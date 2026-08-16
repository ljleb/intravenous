#include "intravenous/channel_ports.h"
#include "intravenous/channel_layout.h"
#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/node/layout.h>
#include <intravenous/ports.h>

using namespace iv;

struct FunNode
{
    static constexpr auto outputs()
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
            s = s * 0.99999;
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

    auto make_channel = [&]<auto c>()
    {
        auto f = g.node<Constant>(220);
        auto const voice = g.node<SawOscillator>();

        NodeRef p;
        if constexpr (c == stereo::left)
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

        auto const res = voice * 0.1 * tt;
        auto const res_k = "main1"_P;
        g.outputs(res_k[c] = res, res_k[swap_side(c)] = res);
    };
    make_channel.template operator()<stereo::left>();
    make_channel.template operator()<stereo::right>();

    g.outputs();
}
}

IV_EXPORT_MODULE("iv.project.simple_sine", simple_sine);
