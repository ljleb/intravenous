#include <intravenous/dsl.h>

namespace {
    constexpr void duplicate_two(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
            auto const sink = context.target_factory().sink(g, channel);
            sink(0.0f);
        }
        g.outputs();
    }
}
