#include "module/module.h"

namespace {
    iv::TypeErasedNode local_cmake_module(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        auto const dt = g.node<iv::ValueSource>(&context.system().sample_period());

        for (size_t channel = 0; channel < context.system().render_config().num_channels; ++channel) {
            auto const sink = context.system().sink(g, channel);
            sink(dt * 0.0f);
        }

        g.outputs();
        return iv::TypeErasedNode(std::move(g).build());
    }
}

IV_EXPORT_MODULE(local_cmake_module);
