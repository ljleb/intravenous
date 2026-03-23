#include "module/module.h"

namespace {
    iv::TypeErasedNode build_failure(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        g.outputs();
        return iv::TypeErasedNode(std::move(g).build());
    }
}

IV_EXPORT_MODULE("iv.test.build_failure", build_failure);

this will not compile
