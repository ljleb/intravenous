#include "dsl.h"

namespace {
    void build_failure(iv::ModuleContext const& context)
    {
        auto& g = context.builder();
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.build_failure", build_failure);

this will not compile
