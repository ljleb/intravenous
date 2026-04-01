#include "module/module.h"

namespace {
    void duplicate_one(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
            auto const sink = context.target_factory().sink(g, channel);
            sink(0.0f);
        }
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.duplicate", duplicate_one);
