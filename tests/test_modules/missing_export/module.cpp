#include <intravenous/dsl.h>

namespace {
    using iv::operator""_P;

    iv::TypeErasedNode missing_export_module(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        g.outputs(
            "main"_P[iv::stereo::left] = 0.0f,
            "main"_P[iv::stereo::right] = 0.0f
        );
        return iv::TypeErasedNode(g.build_root_node().graph);
    }
}
