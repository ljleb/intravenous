#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>

namespace {
    using iv::operator""_P;

    void local_cmake_module(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        auto const dt = g.node<iv::ValueSource>(&context.sample_period());
        g.outputs(
            "main"_P[iv::stereo::left] = dt * 0.0f,
            "main"_P[iv::stereo::right] = dt * 0.0f
        );
    }
}

IV_EXPORT_MODULE("iv.test.local_cmake", local_cmake_module);
