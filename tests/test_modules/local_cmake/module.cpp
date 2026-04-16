#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    void local_cmake_module(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        auto const dt = NODE(g, iv::ValueSource, &context.sample_period());

        for (size_t channel = 0; channel < context.render_config().num_channels; ++channel) {
            auto const sink = context.target_factory().sink(g, channel);
            sink(dt * 0.0f);
        }

        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.local_cmake", local_cmake_module);
