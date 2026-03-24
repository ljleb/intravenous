#include "module/module.h"

namespace iv {
    void missing_dependency(ModuleContext const& context)
    {
        auto& g = context.builder();
        auto dep = context.load("iv.test.this_does_not_exist");
        auto const voice = g.node(dep.builder());
        g.outputs(voice);
    }
}

IV_EXPORT_MODULE("iv.test.missing_dependency", missing_dependency);
