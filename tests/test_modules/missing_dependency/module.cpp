#include "dsl.h"

namespace iv {
    void missing_dependency(ModuleContext const& context)
    {
        auto& g = context.builder();
        auto const voice = context.load("iv.test.this_does_not_exist");
        g.outputs("out"_P = voice);
    }
}

IV_EXPORT_MODULE("iv.test.missing_dependency", missing_dependency);
