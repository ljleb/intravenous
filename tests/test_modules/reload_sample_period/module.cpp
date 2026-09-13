#include <intravenous/dsl.h>

#include <array>

namespace {
struct SamplePeriodSource {
    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 1>{};
    }

    void tick(iv::TickSampleContext<SamplePeriodSource> const& ctx) const
    {
        ctx.outputs[0].push(ctx.sample_period());
    }
};
}

void reload_sample_period(iv::GraphBuilder& g)
{
    g.outputs(g.node<"iv.test.reload_sample_period.source">());
}

IV_NODE("iv.test.reload_sample_period.source", SamplePeriodSource);
IV_MODULE("iv.test.reload_sample_period", reload_sample_period);
