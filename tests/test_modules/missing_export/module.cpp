#include <intravenous/dsl.h>

namespace {
    iv::TypeErasedNode missing_export_module(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        g.outputs(
            iv::channels::stereo_left = 0.0f,
            iv::channels::stereo_right = 0.0f
        );
        return iv::TypeErasedNode(g.build_root_node().graph);
    }
}
