#include "module/module.h"

namespace {
    void duplicate_two(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        for (size_t channel = 0; channel < context.system().render_config().num_channels; ++channel) {
            auto const sink = context.system().sink(g, channel);
            sink(0.0f);
        }
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.duplicate", duplicate_two);
