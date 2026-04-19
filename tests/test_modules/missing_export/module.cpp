#include "dsl.h"

namespace {
    iv::TypeErasedNode missing_export_module(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
            auto const sink = context.target_factory().sink(g, channel);
            sink(0.0f);
        }

        g.outputs();
        return iv::TypeErasedNode(g.plan().build());
    }
}
