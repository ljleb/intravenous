#include "module/module.h"

namespace {
    iv::TypeErasedNode missing_dependency(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        auto dep = context.load("iv.test.this_does_not_exist");
        auto const voice = g.node<iv::TypeErasedNode>(dep.build(context));
        g.outputs(voice);
        return iv::TypeErasedNode(std::move(g).build());
    }
}

IV_EXPORT_MODULE("iv.test.missing_dependency", missing_dependency);
