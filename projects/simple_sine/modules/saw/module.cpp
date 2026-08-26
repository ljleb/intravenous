#include "intravenous/channel_ports.h"
#include "intravenous/channel_layout.h"
#include <intravenous/dsl.h>
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

constexpr void simple_sine(iv::GraphBuilder& g)
{
    // auto const phase = g.node<PhaseIntegrator>();
    // auto const tt = g.node<FunNode>();
    auto const f = g.node<Constant, stereo>(220);
    auto const voice = g.node<SawOscillator, stereo>();
    auto const p = g.tile<stereo>(f + 2.5, f - 2.5);

    voice(
        "frequency"_P = p);
        // "phase_offset"_P = phase);

    // auto const res = voice * g.node<Constant, stereo>(0.1);// * tt;
    g.outputs(
        "main"_P[stereo::left] = voice[stereo::left] * 0.1,
        "main"_P[stereo::right] = voice[stereo::right] * 0.1);
}
