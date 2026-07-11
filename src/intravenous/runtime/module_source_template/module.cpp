#include <intravenous/dsl.h>

inline void module_main(iv::ModuleContext const& ctx)
{
    using namespace iv;
    auto& g = ctx.builder();
    g.outputs();
}

IV_EXPORT_MODULE("iv.project.template", module_main);
